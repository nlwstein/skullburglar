// Native Vulkan renderer: consumes the D3D runtime's draws (Pm4Executor) with real render-target images instead
// of EDRAM emulation.
//
// Model: every EDRAM surface the guest renders to (base tile, colour/depth format, pitch) is its own image,
// addressed in surface pixels. A resolve (RB_MODECONTROL copy) copies a rectangle of one into a host texture
// keyed by its guest destination address, then applies the requested clears; draws that later sample that
// address get the host texture directly, without a round trip through guest memory.
//
// Draws run the guest shaders through the SDK's Xenos -> SPIR-V translator, which fetches vertices itself from
// guest memory bound as one storage buffer ("shared memory"), so there is no vertex input state.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <tuple>
#include <utility>
#include <vector>

#include <rex/ui/vulkan/mem_alloc.h>

#include "../pm4.h"
#include "shaders.h"
#include "vk_api.h"

namespace bots::gfx::vk {

// Keeps the renderer's GPU copy of guest physical memory current while the game runs (memory_sync.h). Without
// one the first draw uploads all of physical memory once, which is right for a frame from a fixed snapshot
// (bots-replay, shadow samples) and nothing else.
class GuestMemoryTracker {
 public:
  virtual ~GuestMemoryTracker() = default;
  // Makes [start, start + length) current in the shared-memory buffer: re-uploads (UploadGuestMemory) the pages
  // the CPU wrote since they were last requested, and watches them for the next write.
  virtual bool Request(uint32_t start, uint32_t length) = 0;
  // Physical ranges [first, last] the CPU wrote since the last call (for the renderer's caches).
  virtual void TakeInvalidated(std::vector<std::pair<uint32_t, uint32_t>>& ranges) = 0;
};

class Renderer : public Pm4Sink {
 public:
  static constexpr uint32_t kTransientSetCount = 16384;  // descriptor sets per submission

  Renderer(const rex::ui::vulkan::VulkanDevice* device, PhysicalMemory memory);
  ~Renderer() override;
  bool ok() const { return ok_; }

  void OnDraw(const rex::graphics::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps,
              const Draw& draw) override;

  // Submits everything recorded so far and waits for it.
  // Submits what's recorded without waiting (up to kSubmissions in flight). Finish also waits for the GPU:
  // needed before the CPU reads anything the GPU wrote.
  void Flush();
  void Finish();
  // Forgets everything taken from guest memory or rendered (surfaces, textures, the shared-memory copy, stats);
  // keeps shaders, pipelines and samplers. In-process shadow mode renders each sampled frame from scratch.
  void ResetFrame();
  // Live: guest memory changes under the renderer. Set before the first draw.
  void SetMemoryTracker(GuestMemoryTracker* tracker) { tracker_ = tracker; }
  // Pipelines persist in a VkPipelineCache file (the driver checks it was made by the same GPU and driver, and
  // ignores it otherwise). Load merges the file into the cache in use; Save writes it when pipelines were created
  // since the last save.
  // Also loads the pipeline manifest next to it (manifest.cpp).
  void LoadPipelineCache(const std::filesystem::path& path);
  void SavePipelineCache();
  // Pipeline precompile (manifest.cpp). Every draw pipeline created live is appended to a manifest (its shaders'
  // microcode, their translation modifications, the binding counts and the fixed-function state); a later launch
  // creates the recorded pipelines again in the background, a few jobs at a time behind live work, so gameplay finds
  // them ready instead of skipping draws (or stalling, for rect lists) while they compile. Records are guest state
  // plus a device feature class, so manifests from other machines of the same class work too: a read-only master
  // (`<title>.master.bin` next to it, e.g. merged by tools/pipeline_manifest.py) is warmed along with the
  // machine's own. BOTS_GFX_PRECOMPILE=0 turns the warm-up off; recording stays on.
  void LoadManifest(const std::filesystem::path& path);
  // Shader refs (tools/shader_refs.py): a master that names each shader by where it is on the game disc instead of
  // carrying its microcode, so it can ship. `*.refs` files of this device class in `refs_dirs` are warmed like a
  // master, their shaders read through `reader` ("image" = the loaded XEX image, else a path under the game root,
  // offsets into a package's decompressed body) and checked against their hashes. Set before LoadPipelineCache.
  using ShaderSourceReader =
      std::function<bool(const std::string& source, uint32_t offset, uint32_t size, std::vector<uint8_t>& out)>;
  void SetShaderRefs(std::vector<std::filesystem::path> refs_dirs, ShaderSourceReader reader) {
    refs_dirs_ = std::move(refs_dirs), refs_reader_ = std::move(reader);
  }
  // Warm-up progress: pipelines recorded in the manifest, and how many of them are created (ready or failed).
  size_t warmup_total() const { return warm_.size(); }
  size_t warmup_done() const { return warm_done_; }
  // Feeds the warm-up queue and builds the pipelines whose translations finished (manifest.cpp). EndFrame does this
  // every frame; a guest held for the warm-up calls it directly.
  void PumpWarmup();
  // Occlusion queries (occlusion.cpp). The guest brackets a query's draws with EVENT_WRITE_ZPD; at the end D3D
  // leaves 0xFFFFFEED markers in the sample-count struct at `address` and waits for the GPU to replace them. Each
  // draw in between is counted by a Vulkan occlusion query, and a resolver thread writes the sum into the guest
  // struct once the GPU has finished that submission. A query the renderer can't count exactly (a draw skipped or
  // split across submissions, no draws, out of slots) reports `visible_samples` at once.
  void EnableOcclusionQueries(uint32_t visible_samples);
  // On by default, except on Intel: the T480 (GPU-bound) gains no GPU time from the culled draws and loses the
  // CPU/GPU overlap while the guest waits for results (14.95 vs 15.5 fps). BOTS_GFX_OCCLUSION=0/1 overrides.
  bool occlusion_queries_by_default() const;
  bool occlusion_queries() const { return occlusion_pool_size_ != 0; }
  void BeginOcclusionQuery();
  void EndOcclusionQuery(uint32_t address);
  // Queries recorded but not submitted: the guest may be waiting for them (submit when the command stream runs dry).
  bool occlusion_unsubmitted() const;
  uint64_t occlusion_hidden() const { return occlusion_hidden_.load(std::memory_order_relaxed); }
  // For the tracker: records a copy of guest physical [start, start + length) into the shared-memory buffer.
  bool UploadGuestMemory(uint32_t start, uint32_t length);
  // Live: after each guest frame's Flush. Evicts guest textures no draw used for a while.
  void EndFrame();
  // Native present: the front buffer resolved to `address` (else the latest resolve), through the 256-entry
  // gamma ramp (10:10:10 per entry, red highest), into `target`: the presenter's guest output image
  // (A2B10G10R10), left in SHADER_READ_ONLY_OPTIMAL as the presenter expects. Submits and waits.
  bool Present(uint32_t address, VkImage target, VkImageView target_view, uint32_t width, uint32_t height,
               const uint32_t* gamma_ramp);

  // Guest address of the last resolve destination (the frame ends with a resolve into the front buffer).
  uint32_t last_resolve_address() const { return last_resolve_address_; }
  // Reads the host texture resolved to `address` back as 8-bit RGB (Flush first).
  bool ReadTexture(uint32_t address, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height);

  struct Stats {
    uint64_t draws = 0, resolves = 0, clears = 0, skipped_draws = 0, dropped_resolves = 0, msaa_resolves = 0;
    uint64_t render_passes = 0, pipelines = 0, failed_shaders = 0, unsupported_primitives = 0;
    uint64_t textures = 0, unsupported_textures = 0, transfers = 0;
    uint64_t uploaded_bytes = 0, invalidations = 0, texture_reloads = 0, texture_evictions = 0;  // live
    uint64_t presents = 0, barriers = 0;  // native
    uint64_t constant_reuses = 0;  // constant buffers bound from an earlier draw's upload
    uint64_t index_reuses = 0, index_converts = 0;  // DMA index buffers bound from an earlier conversion / converted
    uint64_t pending_draws = 0;  // live: skipped while their shaders or pipeline were still being made
    // Occlusion queries ended, and those answered "visible" without counting (occlusion_hidden(): counted 0).
    uint64_t occlusion_queries = 0, occlusion_inexact = 0;
    uint64_t draw_phase_ns[12] = {};  // BOTS_DRAW_PROFILE=1: IssueDraw's time by phase (kDrawPhaseNames)
    // Profiling (P5): submits, and microseconds blocked on the GPU fence, inside OnDraw (recording, including
    // texture loads and any flush it triggers) and inside LoadTexture (untile + upload).
    uint64_t flushes = 0, wait_us = 0, draw_us = 0, texture_us = 0;
    uint64_t gpu_ns[7] = {};  // BOTS_GPU_TIMING=1: GPU time per GpuCat
  };
  const Stats& stats() const { return stats_; }

 private:
  // Adds the scope's duration in microseconds to `total`.
  struct ScopedMicros {
    explicit ScopedMicros(uint64_t& total) : total(total), start(std::chrono::steady_clock::now()) {}
    ~ScopedMicros() {
      total += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                            .count());
    }
    uint64_t& total;
    std::chrono::steady_clock::time_point start;
  };

  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, depth = 1, layers = 1, levels = 1;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    uint64_t written = 0;  // surfaces and resolve destinations: write_clock_ at the last write
    VkImageView depth_view = VK_NULL_HANDLE, stencil_view = VK_NULL_HANDLE;  // depth surfaces, for sampling
    std::map<uint32_t, VkImageView> swizzled_views;  // sampled images: by host swizzle | other signedness << 12
    bool swapped = false;  // resolve destinations: red and blue swapped (RB_COPY_DEST_INFO copy_dest_swap)
    // Surfaces reach to the end of EDRAM (often 1328+ rows for a 720-row target), and on tiling GPUs every render
    // pass loads and stores its whole render area. used_*: the extent from the origin that anything has written
    // (format transfers copy only that). area_*: the same over viewport-bounded draws only, the render area a pass
    // starts with; the few screen-space draws (VTE off: viewport as big as the surface) reopen their pass larger.
    uint32_t used_width = 0, used_height = 0, area_width = 0, area_height = 0;
    void Use(uint32_t x1, uint32_t y1, bool hint = true) {
      x1 = std::min(x1, width), y1 = std::min(y1, height);
      used_width = std::max(used_width, x1), used_height = std::max(used_height, y1);
      if (hint) area_width = std::max(area_width, x1), area_height = std::max(area_height, y1);
    }
    // Depth surfaces: the colour targets of the last pass with colour that used this depth (pass merging,
    // draw.cpp). Surfaces are only destroyed all together.
    std::array<Image*, 4> companions{};
    // A depth resolve's raw 8888 image, not written yet: derived from this depth-value image (same size, rect
    // raw_depth_rect) when first sampled. Games mostly sample only the value (UE3: every shadow map), so a depth
    // resolve writes just that. The stencil byte reads as 0 (Xenos shaders cannot export stencil anyway).
    Image* raw_depth_from = nullptr;
    bool raw_depth_float = false;
    VkRect2D raw_depth_rect{};
  };
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    uint8_t* mapping = nullptr;
    VkDeviceSize size = 0;
  };
  // An EDRAM surface: base tile, depth?, guest format, pitch in pixels.
  using SurfaceKey = std::tuple<uint32_t, bool, uint32_t, uint32_t>;
  // A resolve destination: guest address, guest texture format, width, height.
  using TextureKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
  // Texture/sampler binding counts: vertex textures, vertex samplers, pixel textures, pixel samplers.
  using LayoutKey = std::array<uint32_t, 4>;
  struct PipelineLayout {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_vertex = VK_NULL_HANDLE, textures_pixel = VK_NULL_HANDLE;
  };
  // A guest texture's host image (null image if unsupported) and its views by host swizzle.
  struct GuestTexture {
    Image image;
    uint32_t host_format_swizzle = 0;
    // Guest physical memory it was loaded from: [start, end) of the base level and of the mips.
    std::array<std::pair<uint32_t, uint32_t>, 2> ranges{};
    bool dirty = false;  // the CPU wrote that memory since (tracked memory)
    uint64_t last_used = 0;  // frame_
    // The controller-button atlas (B-105): reloaded when the prompt device changes (PromptState generation).
    bool prompt_atlas = false;
    uint32_t prompt_generation = 0;
  };
  using TextureCacheKey = std::array<uint32_t, 5>;
  // Filters (mag, min, mip), clamp modes (x, y, z), border colour, mip range, anisotropy.
  using SamplerKey = std::array<uint32_t, 10>;
  // Hashes for the per-draw lookups (hash maps rather than ordered maps: B-085).
  struct WordsHash {
    template <size_t N>
    size_t operator()(const std::array<uint32_t, N>& words) const {
      uint64_t h = 0x9E3779B97F4A7C15ull;
      for (uint32_t w : words) h = (h ^ w) * 0xFF51AFD7ED558CCDull, h ^= h >> 32;
      return size_t(h);
    }
  };
  struct BytesHash {  // std::string keys looked up by std::string_view without a temporary string
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };
  // The attachments of the open render pass.
  struct Attachments {
    std::array<Image*, 4> color{};
    Image* depth = nullptr;
    bool operator==(const Attachments&) const = default;
  };

  void DestroyImage(Image& image);
  // Images still referenced by recorded commands: destroyed after the next Flush.
  void Retire(Image& image);
  // Applies the tracker's invalidated ranges to the texture caches.
  void ProcessInvalidations();
  void DestroyFrameState();
  Image* Surface(const rex::graphics::RegisterFile& regs, bool depth, uint32_t color_index);
  // Host pixels per guest pixel of the current surface (its MSAA sample grid).
  static void SampleGrid(const rex::graphics::RegisterFile& regs, uint32_t& x, uint32_t& y);
  // Render scale (RenderScale()): a guest coordinate (in samples) -> host pixels. Edges map through the same
  // rounding everywhere, so rectangles that tile in guest pixels still tile on the host.
  float scale_ = 1.0f;
  uint32_t Host(uint32_t v) const { return scale_ == 1.0f ? v : uint32_t(float(v) * scale_ + 0.5f); }
  int32_t Host(int32_t v) const { return int32_t(Host(uint32_t(std::max(v, 0)))); }
  bool ReadImage(const Image& image, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height);
  void DumpResolve(const Image& image, uint32_t address);
  Image* Own(const SurfaceKey& key, Image* image);
  Image* ResolveTexture(uint32_t address, uint32_t format, uint32_t width, uint32_t height, bool depth_value = false,
                        bool is_signed = false);
  // depth_or_layers: Z size of 3D images, array layers otherwise (6 for cubes).
  bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, bool depth,
                   VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D, bool attachment = true,
                   uint32_t depth_or_layers = 1, uint32_t levels = 1);
  // host: mapped for CPU writes (sequential); otherwise GPU-only, filled by copies.
  bool CreateBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host = true);
  void Resolve(const rex::graphics::RegisterFile& regs);
  void Barrier();
  // Barriers only on a hazard: images written / read by GPU work recorded since the last Barrier(). Access()
  // barriers if this work's reads or writes conflict with those, then records them; rendering must be ended.
  bool Conflicts(std::span<Image* const> reads, std::span<Image* const> writes) const;
  void Track(std::span<Image* const> reads, std::span<Image* const> writes);
  void Access(std::span<Image* const> reads, std::span<Image* const> writes);
  std::vector<const Image*> written_since_barrier_, read_since_barrier_;
  std::vector<Image*> draw_reads_;  // resolve destinations the current draw samples
  // GPU timing (BOTS_GPU_TIMING=1): a timestamp whenever the kind of work being recorded changes; each span is
  // added to stats_.gpu_ns[kind] once its submission completes. BeginCommands marks its caller's kind.
 public:
  enum DrawPhase : uint8_t {
    kPhaseSetup, kPhaseShaders, kPhaseTracker, kPhaseTextures, kPhaseRing, kPhaseTargets, kPhaseIndices,
    kPhaseConstants, kPhaseDescriptors, kPhasePipeline, kPhasePass, kPhaseRecord
  };
  static constexpr const char* kDrawPhaseNames[] = {"setup", "shaders", "tracker", "textures", "ring", "targets",
                                                    "indices", "constants", "descriptors", "pipeline", "pass",
                                                    "record"};
  bool draw_profile() const { return draw_profile_; }
  float render_scale() const { return scale_; }
  // The guest output image's size relative to the frame buffer: above render scale 1 the presenter gets the
  // rendered size (as the emulated renderer's resolution scale does); below 1 the present pass upscales to 1.
  float output_scale() const { return std::max(scale_, 1.0f); }
  enum GpuCat : uint8_t { kGpuDraw, kGpuResolve, kGpuUpload, kGpuTexture, kGpuTransfer, kGpuPresent, kGpuOther };
  static constexpr const char* kGpuCatNames[] = {"draw", "resolve", "upload", "texture", "transfer", "present",
                                                 "other"};
  bool gpu_timing() const { return gpu_timing_; }
  // The labelled spans with the most GPU time since the last call ("label ms/frame xcount", one per line).
  std::string TakeGpuLabelReport(double frames, size_t top);
  // Memory per Vulkan heap (ours via VMA, and the process's where the driver reports a budget), the textures held,
  // and the most of a submission's upload ring used since the last call (B-113).
  std::string TakeMemoryReport();

 private:
  void GpuMark(GpuCat cat, bool force = false);
  void GpuLabel(GpuCat cat, std::string label);  // a forced mark whose span is reported under its label
  std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> gpu_labels_;  // ns, spans
  static constexpr uint32_t kMaxGpuMarks = 16384;
  bool gpu_timing_ = false;
  uint8_t gpu_cat_ = 0xFF;
  double timestamp_ns_ = 1.0;
  void BeginCommands(GpuCat cat = kGpuOther);
  // why: what ended the pass (BOTS_TRACE_RENDERING=1 prints it).
  void EndRendering(const char* why = "other");
  // vkCmdBeginRendering; BOTS_TRACE_RENDERING=1 prints each one first (flushed: for driver crashes).
  void BeginRendering(const VkRenderingInfo& info, const char* what);
  VkImageView DummyDepth(uint32_t width, uint32_t height);  // at least this size
  // Pipelines declare a depth format even without depth (see dummy_depth_).
  VkFormat PassDepthFormat(VkFormat depth) const { return depth || !dummy_depth_ ? depth : VK_FORMAT_D32_SFLOAT_S8_UINT; }

  // Draws (draw.cpp).
  // Adds the time since the last Mark to the current phase's total, then moves on to `next` (no-op without totals).
  struct DrawProfile {
    uint64_t* totals;
    uint8_t phase = kPhaseSetup;
    std::chrono::steady_clock::time_point last;
    explicit DrawProfile(uint64_t* totals) : totals(totals) {
      if (totals) last = std::chrono::steady_clock::now();
    }
    void Mark(uint8_t next) {
      if (!totals) return;
      const auto now = std::chrono::steady_clock::now();
      totals[phase] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
      last = now, phase = next;
    }
    ~DrawProfile() { Mark(phase); }
  };
  bool draw_profile_ = false;
  bool trace_rendering_ = false;
  bool pass_merge_ = true;
  bool lazy_raw_depth_ = true;  // BOTS_GFX_LAZY_RAW_DEPTH=0: write a depth resolve's raw 8888 image at once (A/B)
  uint32_t max_anisotropy_ = 16;  // BOTS_GFX_MAX_ANISO=0|1|2|4|8|16: cap on the game's anisotropic filtering (0 = off)
  bool gpu_draw_labels_ = false;  // BOTS_GPU_TIMING=2: a span per draw, by shader pair
  bool InitDrawing();
  void DestroyDrawing();
  bool UploadSharedMemory();
  void IssueDraw(const rex::graphics::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps,
                 const Draw& draw);
  bool RectListBound(const rex::graphics::RegisterFile& regs, const ShaderCache::Shader& vshader,
                     rex::graphics::xenos::SourceSelect source, uint32_t count, uint32_t& x1, uint32_t& y1) const;
  // Space in this submission's upload ring (uniforms and indices); flushes when full.
  uint8_t* Upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset);
  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);
  VkDescriptorSetLayout TextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  const PipelineLayout* GetPipelineLayout(const LayoutKey& key);
  // Everything that selects a draw pipeline: translations (ShaderCache::Translation*), layout, then fixed-function
  // state (plain values from `topology` on; the manifest stores those bytes as they are). Zero-initialised (padding
  // included) and compared as bytes. A pipeline warmed from the master isn't recorded again in this machine's
  // manifest, which so holds what this machine needed beyond the master (what intake should merge back).
  struct PipelineKey {
    const void* vs;
    const void* ps;
    VkPipelineLayout layout;
    uint32_t topology, restart, cull, front_clockwise, polygon_line, depth_clamp;
    uint32_t depth_test, depth_write, depth_op, stencil, front[4], back[4];
    VkFormat color[4], depth;
    uint32_t blend[4], write_mask[4];
  };
  VkPipeline CreateDrawPipeline(const PipelineKey& key) const;
  // The previous draw's pipeline is checked first (consecutive draws mostly share it). Not yet created: created now
  // (sync), or queued on a worker and null with `pending` set. `background`: queued behind live work (warm-up).
  VkPipeline GetPipeline(const PipelineKey& key, bool sync, bool& pending, bool background = false);
  // Manifest (manifest.cpp): append a newly created live pipeline.
  void RecordPipeline(const PipelineKey& key);

  // EDRAM ownership transfers (transfer.cpp).
  static constexpr uint32_t kTransferDepth = 100;       // Transfer source format for D24S8 depth/stencil
  static constexpr uint32_t kTransferFloatDepth = 101;  // ... and for D24FS8 (20e4 depth)
  static constexpr uint32_t kTransferValueDepth = 102;  // Transfer source: a depth-value image, encoded as D24
  static constexpr uint32_t kTransferValueFloatDepth = 103;  // ... and as 20e4
  static constexpr uint32_t kTransferDepthValue = 200;  // Transfer destination: the depth value as R32_SFLOAT
  static constexpr uint32_t kTransferResolve = 300;     // Transfer destination: a colour resolve (exponent bias)
  static bool IsTransferable(uint32_t format);
  bool InitTransfers();
  void DestroyTransfers();
  VkShaderModule CreateShaderModule(int stage, const std::string& source);
  VkPipeline TransferPipeline(uint32_t source_format, uint32_t dest_format, VkFormat host_format);
  // A full-screen triangle (transfer_vs_) into one colour attachment, dynamic viewport and scissor.
  VkPipeline FullscreenPipeline(VkShaderModule fs, VkFormat format, VkPipelineLayout layout);
  bool InitPresent();
  void Transfer(Image& source, uint32_t source_format, Image& dest, uint32_t dest_format,
                const VkRect2D* rect = nullptr, float scale = 1.0f);

  // Textures (texture.cpp). TextureView may record an upload, so call it before a draw's ring reservation.
  // Views for the translator's unsigned or signed binding of a fetch: formats with both signednesses get a view of
  // the other one over the same bits (hardware reads 16-bit EDRAM resolves as unorm when the fetch says unsigned).
  static VkFormat SignTwin(VkFormat format, bool* is_signed = nullptr);
  VkImageView TextureView(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, bool is_signed,
                          uint32_t& host_swizzle);
  // The resolve destination a fetch reads, if any (a depth resolve read as 24_8 gives the depth value).
  Image* ResolvedTexture(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);
  // A view of a resolve destination as the fetch constant sees it (swizzle; red/blue swap).
  Image* Crop(Image& image, uint32_t width, uint32_t height);
  void MaterializeRawDepth(Image& raw);  // writes a lazy depth resolve's raw image (Image::raw_depth_from)
  VkImageView ResolvedTextureView(Image& image, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                                  bool is_signed, uint32_t& host_swizzle);
  VkImageView SwizzledView(Image& image, uint32_t host_swizzle, bool is_signed);
  void LoadTexture(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, GuestTexture& texture);
  bool LoadPromptAtlas(GuestTexture& texture, uint32_t address);
  VkSampler GetSampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                       const rex::graphics::SpirvShader::SamplerBinding& binding);

  const rex::ui::vulkan::VulkanDevice* device_;
  PhysicalMemory memory_;
  GuestMemoryTracker* tracker_ = nullptr;
  std::vector<Image> retired_;  // the current submission's
  uint64_t frame_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> invalidated_;
  Api vk_;
  bool ok_ = false;
  VmaAllocator allocator_ = nullptr;
  struct OcclusionQuery {
    uint32_t address, first, count;  // guest sample-count struct; slots in the submission's pool
    uint64_t generation;             // occlusion_generations_[address] when it ended
  };
  // Submissions in flight. The members below (pool_, cmd_, fence_, ring_, transient_pool_, retired_) are the
  // current one's; Flush submits it and moves on to the next slot, waiting only if that slot is still pending.
  struct Submission {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool transient_pool = VK_NULL_HANDLE;
    std::vector<Image> retired;
    bool pending = false;
    VkQueryPool timing_pool = VK_NULL_HANDLE;
    std::vector<uint8_t> marks;  // GpuCat per timestamp written
    std::vector<std::string> labels;  // parallel to marks
    // Occlusion queries: one slot per counted draw; the guest queries they sum into, resolved after the fence.
    VkQueryPool occlusion_pool = VK_NULL_HANDLE;
    uint32_t occlusion_slots = 0;           // used in this recording
    uint32_t occlusion_reset = UINT32_MAX;  // slots to reset when recording begins (all, at first)
    std::vector<OcclusionQuery> occlusion;
    bool resolving = false;  // queued for the resolver (occlusion_mutex_)
    uint64_t serial = 0;     // stats_.flushes once submitted
  };
  // More slots than frames in flight: occlusion queries add a submission mid-frame (the guest waits on them), and
  // with only 3 slots that one waited for a free slot. EndFrame keeps the frames in flight at kFramesInFlight.
  static constexpr uint32_t kSubmissions = 6, kFramesInFlight = 3;
  std::deque<uint64_t> frame_ends_;  // stats_.flushes at the last EndFrames
  std::array<Submission, kSubmissions> submissions_;
  std::array<Buffer, kSubmissions> rings_;
  uint32_t current_ = 0;
  void UseSubmission(uint32_t index);
  void WaitSubmission(Submission& submission);
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool recording_ = false;

  std::map<SurfaceKey, std::unique_ptr<Image>> surfaces_;
  std::map<TextureKey, std::unique_ptr<Image>> textures_;
  std::unordered_map<uint32_t, Image*> texture_at_;  // guest address -> latest resolve destination there
  // Depth resolves also keep the depth value, for 24_8 fetches (the 8888 destination holds the raw bits).
  std::unordered_map<uint32_t, Image*> depth_texture_at_;
  // Resolve destinations are pitch wide; a fetch of a narrower (or shorter) texture samples a copy of its corner.
  std::map<std::tuple<const Image*, uint32_t, uint32_t>, std::unique_ptr<Image>> crops_;
  uint32_t last_resolve_address_ = 0;
  Stats stats_;

  // Drawing.
  std::unique_ptr<ShaderCache> shaders_;
  Buffer shared_memory_, ring_;
  VkDeviceSize ring_used_ = 0;
  VkDeviceSize ring_peak_ = 0, upload_max_ = 0;  // TakeMemoryReport: most used in a submission, largest upload
  uint64_t ring_full_ = 0;  // uploads that found the ring full and flushed for room
  uint64_t ring_epoch_ = 1;  // bumped whenever ring_ is reset: earlier uploads can no longer be reused
  // The last upload of each constant buffer (B-085). A draw reuses it while the ring is the same and nothing it
  // holds has changed: the same shader (float constants) and no write to its register groups since
  // (Draw::constant_writes; key is the latest write among them), or equal bytes (system constants).
  struct ConstantUpload {
    uint64_t epoch = 0, key = 0;
    const void* shader = nullptr;
    uint32_t offset = 0;
  };
  static constexpr uint32_t kConstantBuffers = 5;
  ConstantUpload constant_uploads_[kConstantBuffers];
  // DMA index buffers converted into the ring this epoch (B-085): a draw with the same indices and format binds the
  // earlier conversion. Entries go when the CPU writes their guest range (ProcessInvalidations).
  struct IndexKey {
    uint32_t base, count, reset_index, flags;
    bool operator==(const IndexKey&) const = default;
  };
  struct IndexKeyHash {
    size_t operator()(const IndexKey& k) const {
      uint64_t h = (uint64_t(k.base) << 32 | k.count) * 0x9E3779B97F4A7C15ull;
      return size_t(h ^ (uint64_t(k.reset_index) << 8 | k.flags) * 0xC2B2AE3D27D4EB4Full);
    }
  };
  struct IndexUpload {
    uint32_t offset, bytes;  // bytes: the guest range read
    bool index32, restart;
  };
  std::unordered_map<IndexKey, IndexUpload, IndexKeyHash> index_uploads_;
  uint64_t index_uploads_epoch_ = 0;
  std::vector<uint8_t> last_system_constants_;
  uint64_t system_constants_version_ = 0;  // bumped when they change
  uint32_t transient_sets_ = 0;  // allocated from transient_pool_ this submission
  bool shared_memory_uploaded_ = false;
  uint32_t shared_memory_bindings_ = 1;
  VkDescriptorSetLayout set_layout_shared_ = VK_NULL_HANDLE, set_layout_constants_ = VK_NULL_HANDLE,
                        set_layout_empty_ = VK_NULL_HANDLE;
  std::map<std::tuple<bool, uint32_t, uint32_t>, VkDescriptorSetLayout> texture_set_layouts_;
  std::map<LayoutKey, PipelineLayout> pipeline_layouts_;
  struct PipelineSlot {
    std::atomic<Async> state{Async::kNone};
    VkPipeline pipeline = VK_NULL_HANDLE;  // set by whoever creates it, before state becomes kReady
    bool counted = false;                  // in stats_.pipelines
    bool recorded = false;                 // passed to RecordPipeline (first live use, however it was created)
  };
  std::unordered_map<std::string, std::unique_ptr<PipelineSlot>, BytesHash, std::equal_to<>> pipelines_;
  bool async_pipelines_ = false;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  bool create_with_cache_ = true;  // false on MoltenVK: pipelines are created without pipeline_cache_ (renderer.cpp)
  bool dummy_depth_ = false;  // every pass gets a depth attachment (driver workaround, renderer.cpp)
  uint32_t log_skips_ = 0;    // BOTS_GFX_LOG_SKIPS=N: log the first N draws skipped for shaders or pipelines
  void LogSkip(const char* why, uint64_t vs, uint64_t ps, const Attachments* attachments);
  std::vector<std::unique_ptr<Image>> dummy_depths_;
  std::filesystem::path pipeline_cache_path_;
  uint64_t pipelines_saved_ = 0;
  // Occlusion queries (occlusion.cpp).
  void ResolveOcclusion();  // the resolver thread
  void WriteOcclusionResult(uint32_t address, uint64_t generation, uint32_t samples);  // occlusion_mutex_ held
  void WaitResolved(uint32_t index);  // until the resolver is done with that submission
  void QueueResolve(uint32_t index);  // just submitted
  void StopOcclusion();
  // Around each draw recorded: count it if a query is open (OnDraw counts the draws that reach the renderer).
  void BeginDrawQuery();
  void EndDrawQuery();
  uint32_t occlusion_pool_size_ = 0, occlusion_visible_ = 1000;
  VkQueryControlFlags occlusion_flags_ = 0;
  bool occlusion_open_ = false, occlusion_drawing_ = false, occlusion_inexact_ = false;  uint64_t occlusion_flush_ = 0;  // stats_.flushes when the open query's slots began
  uint32_t occlusion_first_ = 0, occlusion_draws_ = 0, occlusion_counted_ = 0;
  std::mutex occlusion_mutex_;
  std::condition_variable occlusion_cv_;
  std::deque<uint32_t> occlusion_jobs_;  // submission indices, in submit order
  bool occlusion_stop_ = false;
  std::unordered_map<uint32_t, uint64_t> occlusion_generations_;  // by guest address: queries ended there
  std::thread occlusion_thread_;
  std::atomic<uint64_t> occlusion_hidden_{0};
  // Pipeline manifest (manifest.cpp).
  struct ManifestShader {
    uint32_t type;
    std::vector<uint32_t> ucode;  // big-endian, as in guest memory (so it hashes as it does live)
    ShaderCache::Shader* shader = nullptr;  // once looked up for warm-up
    bool in_file = false;                   // in this machine's manifest (not only the master's)
  };
  struct WarmPipeline {
    std::string record;  // the manifest record
    bool started = false, done = false;
  };
  std::FILE* manifest_ = nullptr;  // open for appending
  std::unordered_map<uint64_t, ManifestShader> manifest_shaders_;  // by ShaderCache hash, read or written
  std::unordered_map<std::string, bool> manifest_records_;         // all known records -> in this machine's file
  std::vector<WarmPipeline> warm_;
  size_t warm_next_ = 0, warm_done_ = 0;
  std::chrono::steady_clock::time_point warm_start_;
  bool warm_enabled_ = true, warm_logged_ = false;
  std::vector<std::filesystem::path> refs_dirs_;
  ShaderSourceReader refs_reader_;
  void LoadShaderRefs(const std::filesystem::path& file, const void* header, size_t header_size);
  std::string last_pipeline_key_;
  VkPipeline last_pipeline_ = VK_NULL_HANDLE;
  // What the current command buffer has bound, so unchanged pipelines aren't rebound (VK_NULL_HANDLE: unknown).
  VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
  // The draw dynamic state last set in the current command buffer, so unchanged state isn't set again (B-085).
  // Invalid after BeginCommands starts a buffer and after a transfer/present pipeline is bound (binding a pipeline
  // with static state disturbs it).
  struct DynamicState {
    VkViewport viewport;
    VkRect2D scissor;
    float bias[2];
    float blend[4];
    uint32_t stencil[6];
  };
  DynamicState dynamic_{};
  bool dynamic_valid_ = false;
  VkDescriptorPool persistent_pool_ = VK_NULL_HANDLE, transient_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_set_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSubmissions> constant_sets_{};  // per ring (draw.cpp)
  VkDescriptorSet constant_set_ = VK_NULL_HANDLE;              // the current ring's
  // This submission's texture sets by layout + bound handles (draw.cpp); cleared with its transient pool.
  std::unordered_map<std::string, VkDescriptorSet, BytesHash, std::equal_to<>> texture_sets_;
  std::string set_key_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  Image null_2d_array_, null_3d_, null_cube_;
  Attachments open_pass_;
  VkExtent2D open_area_{};  // the open pass's render area (from the origin)
  bool rendering_ = false;
  std::unordered_map<TextureCacheKey, std::unique_ptr<GuestTexture>, WordsHash> guest_textures_;
  std::unordered_map<SamplerKey, VkSampler, WordsHash> samplers_;
  bool trace_textures_ = false;
  uint64_t write_clock_ = 0;
  VkDescriptorSetLayout transfer_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout transfer_layout_ = VK_NULL_HANDLE;
  VkShaderModule transfer_vs_ = VK_NULL_HANDLE;
  std::vector<VkShaderModule> transfer_modules_;
  std::map<std::tuple<uint32_t, uint32_t, VkFormat>, VkPipeline> transfer_pipelines_;
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout present_layout_ = VK_NULL_HANDLE;
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace bots::gfx::vk
