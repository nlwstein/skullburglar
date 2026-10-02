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

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
  };
  using TextureCacheKey = std::array<uint32_t, 5>;
  // Filters (mag, min, mip), clamp modes (x, y, z), border colour, mip range, anisotropy.
  using SamplerKey = std::array<uint32_t, 10>;
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
  bool ReadImage(const Image& image, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height);
  void DumpResolve(const Image& image, uint32_t address);
  Image* Own(const SurfaceKey& key, Image* image);
  Image* ResolveTexture(uint32_t address, uint32_t format, uint32_t width, uint32_t height, bool depth_value = false,
                        bool is_signed = false);
  // depth_or_layers: Z size of 3D images, array layers otherwise (6 for cubes).
  bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, bool depth,
                   VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D, bool attachment = true,
                   uint32_t depth_or_layers = 1, uint32_t levels = 1);
  bool CreateBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage);
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
  enum GpuCat : uint8_t { kGpuDraw, kGpuResolve, kGpuUpload, kGpuTexture, kGpuTransfer, kGpuPresent, kGpuOther };
  static constexpr const char* kGpuCatNames[] = {"draw", "resolve", "upload", "texture", "transfer", "present",
                                                 "other"};
  bool gpu_timing() const { return gpu_timing_; }
  // The labelled spans with the most GPU time since the last call ("label ms/frame xcount", one per line).
  std::string TakeGpuLabelReport(double frames, size_t top);

 private:
  void GpuMark(GpuCat cat, bool force = false);
  void GpuLabel(GpuCat cat, std::string label);  // a forced mark whose span is reported under its label
  std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> gpu_labels_;  // ns, spans
  static constexpr uint32_t kMaxGpuMarks = 4096;
  bool gpu_timing_ = false;
  uint8_t gpu_cat_ = 0xFF;
  double timestamp_ns_ = 1.0;
  void BeginCommands(GpuCat cat = kGpuOther);
  void EndRendering();

  // Draws (draw.cpp).
  bool InitDrawing();
  void DestroyDrawing();
  bool UploadSharedMemory();
  void IssueDraw(const rex::graphics::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps,
                 const Draw& draw);
  // Space in this submission's upload ring (uniforms and indices); flushes when full.
  uint8_t* Upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset);
  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);
  VkDescriptorSetLayout TextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  const PipelineLayout* GetPipelineLayout(const LayoutKey& key);
  // Allocation-free on a hit; the previous draw's pipeline is checked first (consecutive draws mostly share it).
  VkPipeline GetPipeline(std::string_view key, const VkGraphicsPipelineCreateInfo& info);

  // EDRAM ownership transfers (transfer.cpp).
  static constexpr uint32_t kTransferDepth = 100;       // Transfer source format for D24S8 depth/stencil
  static constexpr uint32_t kTransferFloatDepth = 101;  // ... and for D24FS8 (20e4 depth)
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
  VkImageView ResolvedTextureView(Image& image, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                                  bool is_signed, uint32_t& host_swizzle);
  VkImageView SwizzledView(Image& image, uint32_t host_swizzle, bool is_signed);
  void LoadTexture(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, GuestTexture& texture);
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
  };
  static constexpr uint32_t kSubmissions = 3;
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
  std::map<uint32_t, Image*> texture_at_;  // guest address -> latest resolve destination there
  // Depth resolves also keep the depth value, for 24_8 fetches (the 8888 destination holds the raw bits).
  std::map<uint32_t, Image*> depth_texture_at_;
  // Resolve destinations are pitch wide; a fetch of a narrower (or shorter) texture samples a copy of its corner.
  std::map<std::tuple<const Image*, uint32_t, uint32_t>, std::unique_ptr<Image>> crops_;
  uint32_t last_resolve_address_ = 0;
  Stats stats_;

  // Drawing.
  std::unique_ptr<ShaderCache> shaders_;
  Buffer shared_memory_, ring_;
  VkDeviceSize ring_used_ = 0;
  uint32_t transient_sets_ = 0;  // allocated from transient_pool_ this submission
  bool shared_memory_uploaded_ = false;
  uint32_t shared_memory_bindings_ = 1;
  VkDescriptorSetLayout set_layout_shared_ = VK_NULL_HANDLE, set_layout_constants_ = VK_NULL_HANDLE,
                        set_layout_empty_ = VK_NULL_HANDLE;
  std::map<std::tuple<bool, uint32_t, uint32_t>, VkDescriptorSetLayout> texture_set_layouts_;
  std::map<LayoutKey, PipelineLayout> pipeline_layouts_;
  std::unordered_map<std::string, VkPipeline> pipelines_;
  std::string last_pipeline_key_;
  VkPipeline last_pipeline_ = VK_NULL_HANDLE;
  // What the current command buffer has bound, so unchanged pipelines aren't rebound (VK_NULL_HANDLE: unknown).
  VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool persistent_pool_ = VK_NULL_HANDLE, transient_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_set_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  Image null_2d_array_, null_3d_, null_cube_;
  Attachments open_pass_;
  bool rendering_ = false;
  std::map<TextureCacheKey, std::unique_ptr<GuestTexture>> guest_textures_;
  std::map<SamplerKey, VkSampler> samplers_;
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
