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
    uint64_t draws = 0, resolves = 0, clears = 0, skipped_draws = 0, dropped_resolves = 0;
    uint64_t render_passes = 0, pipelines = 0, failed_shaders = 0, unsupported_primitives = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
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
  // The attachments of the open render pass.
  struct Attachments {
    std::array<Image*, 4> color{};
    Image* depth = nullptr;
    bool operator==(const Attachments&) const = default;
  };

  Image* Surface(const rex::graphics::RegisterFile& regs, bool depth, uint32_t color_index);
  Image* ResolveTexture(uint32_t address, uint32_t format, uint32_t width, uint32_t height);
  bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, bool depth,
                   VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D);
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
};

}  // namespace bots::gfx::vk
