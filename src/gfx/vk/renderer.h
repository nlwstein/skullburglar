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
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <rex/ui/vulkan/mem_alloc.h>

#include "../pm4.h"
#include "shaders.h"
#include "vk_api.h"

namespace bots::gfx::vk {

class Renderer : public Pm4Sink {
 public:
  static constexpr uint32_t kTransientSetCount = 16384;  // descriptor sets per submission

  Renderer(const rex::ui::vulkan::VulkanDevice* device, PhysicalMemory memory);
  ~Renderer() override;
  bool ok() const { return ok_; }

  void OnDraw(const rex::graphics::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps,
              const Draw& draw) override;

  // Submits everything recorded so far and waits for it.
  void Flush();

  // Guest address of the last resolve destination (the frame ends with a resolve into the front buffer).
  uint32_t last_resolve_address() const { return last_resolve_address_; }
  // Reads the host texture resolved to `address` back as 8-bit RGB (Flush first).
  bool ReadTexture(uint32_t address, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height);

  struct Stats {
    uint64_t draws = 0, resolves = 0, clears = 0, skipped_draws = 0, dropped_resolves = 0, msaa_resolves = 0;
    uint64_t render_passes = 0, pipelines = 0, failed_shaders = 0, unsupported_primitives = 0;
    uint64_t textures = 0, unsupported_textures = 0, transfers = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
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
  void BeginCommands();
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
  VkPipeline GetPipeline(const std::string& key, const VkGraphicsPipelineCreateInfo& info);

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
  Api vk_;
  bool ok_ = false;
  VmaAllocator allocator_ = nullptr;
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
  std::map<std::string, VkPipeline> pipelines_;
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
};

}  // namespace bots::gfx::vk
