// Native Vulkan renderer: consumes the D3D runtime's draws (Pm4Executor) with real render-target images instead
// of EDRAM emulation.
//
// Model: every EDRAM surface the guest renders to (base tile, colour/depth format, pitch) is its own image,
// addressed in surface pixels. A resolve (RB_MODECONTROL copy) copies a rectangle of one into a host texture
// keyed by its guest destination address, then applies the requested clears; draws that later sample that
// address get the host texture directly, without a round trip through guest memory.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include <rex/ui/vulkan/mem_alloc.h>

#include "../pm4.h"
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
  // An EDRAM surface: base tile, depth?, guest format, pitch in pixels.
  using SurfaceKey = std::tuple<uint32_t, bool, uint32_t, uint32_t>;
  // A resolve destination: guest address, guest texture format, width, height.
  using TextureKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;

  Image* Surface(const rex::graphics::RegisterFile& regs, bool depth, uint32_t color_index);
  Image* ResolveTexture(uint32_t address, uint32_t format, uint32_t width, uint32_t height);
  bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, bool depth);
  void Resolve(const rex::graphics::RegisterFile& regs);
  void Barrier();
  void BeginCommands();

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
};

}  // namespace bots::gfx::vk
