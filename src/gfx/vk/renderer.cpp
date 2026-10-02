#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace bots::gfx::vk {
namespace {

namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

constexpr uint32_t kEdramTiles = 2048;
constexpr uint32_t kTileWidthSamples = 80, kTileHeightSamples = 16;
constexpr uint32_t kMaxSurfaceHeight = 4096;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;  // D24S8 isn't on AMD (Deck)

VkFormat ColorTargetFormat(uint32_t format) {
  switch (xe::ColorRenderTargetFormat(format)) {
    case xe::ColorRenderTargetFormat::k_8_8_8_8:
    case xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xe::ColorRenderTargetFormat::k_2_10_10_10:
    case xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xe::ColorRenderTargetFormat::k_16_16:
      return VK_FORMAT_R16G16_SNORM;
    case xe::ColorRenderTargetFormat::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case xe::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xe::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xe::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:  // 2_10_10_10_FLOAT (7e3) and the 16-bit float formats
      return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}

// Resolve destination (guest texture) format and signedness (RB_COPY_DEST_INFO copy_dest_number) -> host format.
VkFormat TextureFormat(uint32_t format, bool is_signed) {
  switch (xe::TextureFormat(format)) {
    case xe::TextureFormat::k_8:
      return is_signed ? VK_FORMAT_R8_SNORM : VK_FORMAT_R8_UNORM;
    case xe::TextureFormat::k_8_8_8_8:
    case xe::TextureFormat::k_8_8_8_8_A:
      return is_signed ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
    case xe::TextureFormat::k_2_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xe::TextureFormat::k_10_11_11:
    case xe::TextureFormat::k_11_11_10:
      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case xe::TextureFormat::k_16_16:
      return is_signed ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
    case xe::TextureFormat::k_16_16_16_16:
      return is_signed ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM;
    case xe::TextureFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xe::TextureFormat::k_24_8:
    case xe::TextureFormat::k_24_8_FLOAT:
    case xe::TextureFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xe::TextureFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case xe::TextureFormat::k_32_32_32_32_FLOAT:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}

float Unorm(uint32_t v, uint32_t bits) { return float(v & ((1u << bits) - 1)) / float((1u << bits) - 1); }

// Xenos 7e3: 3-bit exponent (bias 3), 7-bit mantissa, [0, 31.875].
float Float7e3(uint32_t v) {
  const uint32_t e = (v >> 7) & 7, m = v & 0x7F;
  return e ? std::ldexp(1.0f + float(m) / 128.0f, int(e) - 3) : std::ldexp(float(m) / 128.0f, -2);
}

// Xenos 20e4 depth: 4-bit exponent (bias 15), 20-bit mantissa, [0, 2).
float Float20e4(uint32_t v) {
  const uint32_t e = (v >> 20) & 0xF, m = v & 0xFFFFF;
  return e ? std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15) : std::ldexp(float(m) / 1048576.0f, -14);
}

float Half(uint16_t h) {
  const uint32_t s = h >> 15, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  float f = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 0x400), int(e) - 25);
  return s ? -f : f;
}

// RB_COLOR_CLEAR (+ _LO for 64bpp) is the raw EDRAM value of the cleared pixel.
VkClearColorValue ColorClear(uint32_t format, uint32_t lo, uint32_t hi) {
  VkClearColorValue c{};
  auto snorm16 = [](uint32_t v) { return std::max(-1.0f, float(int16_t(v & 0xFFFF)) / 32767.0f); };
  switch (xe::ColorRenderTargetFormat(format)) {
    case xe::ColorRenderTargetFormat::k_8_8_8_8:
    case xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      c.float32[0] = Unorm(lo, 8), c.float32[1] = Unorm(lo >> 8, 8);
      c.float32[2] = Unorm(lo >> 16, 8), c.float32[3] = Unorm(lo >> 24, 8);
      break;
    case xe::ColorRenderTargetFormat::k_2_10_10_10:
    case xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      c.float32[0] = Unorm(lo, 10), c.float32[1] = Unorm(lo >> 10, 10);
      c.float32[2] = Unorm(lo >> 20, 10), c.float32[3] = Unorm(lo >> 30, 2);
      break;
    case xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      c.float32[0] = Float7e3(lo), c.float32[1] = Float7e3(lo >> 10);
      c.float32[2] = Float7e3(lo >> 20), c.float32[3] = Unorm(lo >> 30, 2);
      break;
    case xe::ColorRenderTargetFormat::k_16_16:
      c.float32[0] = snorm16(lo), c.float32[1] = snorm16(lo >> 16);
      break;
    case xe::ColorRenderTargetFormat::k_16_16_16_16:
      c.float32[0] = snorm16(lo), c.float32[1] = snorm16(lo >> 16);
      c.float32[2] = snorm16(hi), c.float32[3] = snorm16(hi >> 16);
      break;
    case xe::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xe::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      c.float32[0] = Half(lo), c.float32[1] = Half(lo >> 16), c.float32[2] = Half(hi), c.float32[3] = Half(hi >> 16);
      break;
    case xe::ColorRenderTargetFormat::k_32_FLOAT:
    case xe::ColorRenderTargetFormat::k_32_32_FLOAT:
      std::memcpy(&c.float32[0], &lo, 4);
      std::memcpy(&c.float32[1], &hi, 4);
      break;
  }
  return c;
}

uint8_t ToByte(float f) { return uint8_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

Renderer::Renderer(const rex::ui::vulkan::VulkanDevice* device, PhysicalMemory memory)
    : device_(device), memory_(std::move(memory)) {
  if (!device_ || !vk_.Load(*device_) || !device_->properties().dynamicRendering) {
    std::fprintf(stderr, "native renderer: Vulkan device lacks a required function or dynamic rendering\n");
    return;
  }
  allocator_ = rex::ui::vulkan::CreateVmaAllocator(device_, false);
  const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0,
                                          device_->queue_family_graphics_compute()};
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (!allocator_ || vk_.vkCreateCommandPool(device_->device(), &pool_info, nullptr, &pool_) != VK_SUCCESS ||
      vk_.vkCreateFence(device_->device(), &fence_info, nullptr, &fence_) != VK_SUCCESS) {
    return;
  }
  const VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                             VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  ok_ = vk_.vkAllocateCommandBuffers(device_->device(), &cmd_info, &cmd_) == VK_SUCCESS && InitDrawing();
}

Renderer::~Renderer() {
  if (!device_) return;
  const VkDevice dev = device_->device();
  if (vk_.vkDeviceWaitIdle) vk_.vkDeviceWaitIdle(dev);
  auto destroy = [&](Image& image) {
    for (VkImageView view : {image.view, image.depth_view, image.stencil_view}) {
      if (view) vk_.vkDestroyImageView(dev, view, nullptr);
    }
    for (auto& [s, view] : image.swizzled_views) vk_.vkDestroyImageView(dev, view, nullptr);
    if (image.image) vmaDestroyImage(allocator_, image.image, image.allocation);
  };
  for (auto& [k, image] : surfaces_) destroy(*image);
  for (auto& [k, image] : textures_) destroy(*image);
  for (auto& [k, image] : crops_) destroy(*image);
  for (Image* image : {&null_2d_array_, &null_3d_, &null_cube_}) destroy(*image);
  for (auto& [k, texture] : guest_textures_) destroy(texture->image);
  for (auto& [k, sampler] : samplers_) vk_.vkDestroySampler(dev, sampler, nullptr);
  DestroyTransfers();
  DestroyDrawing();
  if (fence_) vk_.vkDestroyFence(dev, fence_, nullptr);
  if (pool_) vk_.vkDestroyCommandPool(dev, pool_, nullptr);
  if (allocator_) vmaDestroyAllocator(allocator_);
}

void Renderer::BeginCommands() {
  if (recording_) return;
  const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  vk_.vkBeginCommandBuffer(cmd_, &begin);
  recording_ = true;
}

// Everything stays in GENERAL layout and every operation is separated by a full barrier. Simple and correct;
// finer barriers are P5 work.
void Renderer::Barrier() {
  const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
  vk_.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                           &barrier, 0, nullptr, 0, nullptr);
}

void Renderer::EndRendering() {
  if (!rendering_) return;
  vk_.vkCmdEndRendering(cmd_);
  rendering_ = false;
}

void Renderer::Flush() {
  if (!recording_) return;
  EndRendering();
  if (ring_used_) vmaFlushAllocation(allocator_, ring_.allocation, 0, ring_used_);
  vk_.vkEndCommandBuffer(cmd_);
  recording_ = false;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd_;
  {
    auto queue = device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
    vk_.vkQueueSubmit(queue.queue(), 1, &submit, fence_);
  }
  vk_.vkWaitForFences(device_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);
  vk_.vkResetFences(device_->device(), 1, &fence_);
  vk_.vkResetCommandPool(device_->device(), pool_, 0);
  vk_.vkResetDescriptorPool(device_->device(), transient_pool_, 0);
  ring_used_ = 0;
  transient_sets_ = 0;
}

bool Renderer::CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, bool depth,
                           VkImageViewType view_type, bool attachment, uint32_t depth_or_layers, uint32_t levels) {
  const bool cube = view_type == VK_IMAGE_VIEW_TYPE_CUBE;
  const bool is_3d = view_type == VK_IMAGE_VIEW_TYPE_3D;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  if (!depth && SignTwin(format) != VK_FORMAT_UNDEFINED) info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  info.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, is_3d ? depth_or_layers : 1};
  info.mipLevels = levels;
  info.arrayLayers = is_3d ? 1 : cube ? std::max(depth_or_layers, 6u) : depth_or_layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (attachment) {
    info.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateImage(allocator_, &info, &alloc, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
    return false;
  }
  image.format = format;
  image.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  image.width = width;
  image.height = height;
  image.depth = info.extent.depth;
  image.layers = info.arrayLayers;
  image.levels = levels;
  image.view_type = view_type;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = view_type;
  view.format = format;
  view.subresourceRange = {image.aspect, 0, levels, 0, info.arrayLayers};
  if (vk_.vkCreateImageView(device_->device(), &view, nullptr, &image.view) != VK_SUCCESS) return false;
  if (depth) {
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk_.vkCreateImageView(device_->device(), &view, nullptr, &image.depth_view) != VK_SUCCESS) return false;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (vk_.vkCreateImageView(device_->device(), &view, nullptr, &image.stencil_view) != VK_SUCCESS) return false;
  }

  BeginCommands();
  EndRendering();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = view.subresourceRange;
  vk_.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
  return true;
}

Renderer::Image* Renderer::Surface(const rg::RegisterFile& regs, bool depth, uint32_t color_index) {
  const auto surface = regs.Get<rg::reg::RB_SURFACE_INFO>();
  uint32_t base, format;
  bool is_64bpp = false;
  if (depth) {
    const auto info = regs.Get<rg::reg::RB_DEPTH_INFO>();
    // Both depth formats are one host format (D32_SFLOAT_S8_UINT); keep a single image per base.
    base = info.depth_base, format = 0;
  } else {
    const auto info = regs.Get<rg::reg::RB_COLOR_INFO>(rg::reg::RB_COLOR_INFO::rt_register_indices[color_index]);
    // Formats that only differ in blending precision share storage; gamma is applied by the shader, so
    // 8_8_8_8_GAMMA holds the same bytes as 8_8_8_8.
    auto storage = xe::GetStorageColorFormat(info.color_format);
    if (storage == xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) storage = xe::ColorRenderTargetFormat::k_8_8_8_8;
    base = info.color_base, format = uint32_t(storage);
    is_64bpp = xe::IsColorRenderTargetFormat64bpp(info.color_format);
  }
  // Multisampled surfaces are rendered supersampled: one host pixel per guest sample. A 4x surface then is the
  // same image as a 1x surface of twice the pitch over the same tiles, which the XDK relies on (it clears an
  // 880-wide depth buffer by drawing a 440-wide 4x one).
  uint32_t samples_x, samples_y;
  SampleGrid(regs, samples_x, samples_y);
  const uint32_t pitch = surface.surface_pitch * samples_x;
  if (!pitch) return nullptr;
  const SurfaceKey key{base, depth, format, pitch};
  auto& slot = surfaces_[key];
  if (slot) return Own(key, slot.get());

  // No height on Xenos: the surface can extend to the end of EDRAM.
  const uint32_t pitch_tiles = ((pitch + kTileWidthSamples - 1) / kTileWidthSamples) << is_64bpp;
  const uint32_t rows = std::max(1u, (kEdramTiles - base % kEdramTiles) / pitch_tiles);
  const uint32_t height = std::min(rows * kTileHeightSamples, kMaxSurfaceHeight);
  auto image = std::make_unique<Image>();
  if (!CreateImage(*image, depth ? kDepthFormat : ColorTargetFormat(format), pitch, height, depth)) return nullptr;
  slot = std::move(image);
  return Own(key, slot.get());
}

void Renderer::SampleGrid(const rg::RegisterFile& regs, uint32_t& x, uint32_t& y) {
  const auto samples = regs.Get<rg::reg::RB_SURFACE_INFO>().msaa_samples;
  x = samples >= xe::MsaaSamples::k4X ? 2 : 1;
  y = samples >= xe::MsaaSamples::k2X ? 2 : 1;
}

// If another format's surface at the same base and pitch was written more recently, takes over its contents.
Renderer::Image* Renderer::Own(const SurfaceKey& key, Image* image) {
  const auto [base, depth, format, pitch] = key;
  if (depth || !IsTransferable(format)) return image;
  Image* newest = nullptr;
  uint32_t newest_format = 0;
  for (auto& [other_key, other] : surfaces_) {
    const auto [other_base, other_depth, other_format, other_pitch] = other_key;
    // Colour surfaces of another format at the same base and pitch, and depth surfaces at the same base (the XDK
    // clears colour tiles with depth-only draws).
    const bool candidate = other_depth ? other_base == base
                                       : other_base == base && other_pitch == pitch && other_format != format &&
                                             IsTransferable(other_format);
    if (!candidate || other->written <= image->written) continue;
    if (!newest || other->written > newest->written) {
      newest = other.get(), newest_format = other_depth ? kTransferDepth : other_format;
    }
  }
  if (newest) {
    if (trace_textures_) {
      std::fprintf(stderr, "transfer: base %X pitch %u fmt %u -> fmt %u (%ux%u -> %ux%u)\n", base, pitch,
                   newest_format, format, newest->width, newest->height, image->width, image->height);
    }
    Transfer(*newest, newest_format, *image, format);
    image->written = newest->written;
  }
  return image;
}

Renderer::Image* Renderer::ResolveTexture(uint32_t address, uint32_t format, uint32_t width, uint32_t height,
                                          bool depth_value, bool is_signed) {
  const TextureKey key{address, format | (depth_value ? 0x100 : 0) | (is_signed ? 0x200 : 0), width, height};
  auto& slot = textures_[key];
  if (!slot) {
    auto image = std::make_unique<Image>();
    // Shaders sample 2D textures as arrays.
    const VkFormat host_format = depth_value ? VK_FORMAT_R32_SFLOAT : TextureFormat(format, is_signed);
    if (!CreateImage(*image, host_format, width, height, false, VK_IMAGE_VIEW_TYPE_2D_ARRAY)) return nullptr;
    slot = std::move(image);
  }
  if (depth_value) {
    depth_texture_at_[address] = slot.get();
  } else {
    texture_at_[address] = slot.get();
    depth_texture_at_.erase(address);
  }
  return slot.get();
}

void Renderer::OnDraw(const rg::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps, const Draw& draw) {
  if (!ok_) return;
  if (regs.Get<rg::reg::RB_MODECONTROL>().edram_mode == xe::EdramMode::kCopy) {
    Resolve(regs);
    return;
  }
  IssueDraw(regs, vs, ps, draw);
}

void Renderer::Resolve(const rg::RegisterFile& regs) {
  ++stats_.resolves;
  const auto copy = regs.Get<rg::reg::RB_COPY_CONTROL>();
  const bool depth_source = copy.copy_src_select >= xe::kMaxColorRenderTargets;

  // The rectangle: D3D puts its 3 rect-list vertices in vertex fetch 0 (as the SDK's GetResolveInfo).
  const auto fetch = regs.GetVertexFetch(0);
  const uint8_t* vertex_data = memory_(fetch.address * 4);
  if (fetch.type != xe::FetchConstantType::kVertex || fetch.size != 6 || !vertex_data) {
    ++stats_.dropped_resolves;
    return;
  }
  const float half_pixel =
      regs.Get<rg::reg::PA_SU_VTX_CNTL>().pix_center == xe::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t v[6];
  for (int i = 0; i < 6; ++i) {
    uint32_t bits;
    std::memcpy(&bits, vertex_data + 4 * i, 4);
    float f;
    const uint32_t swapped = xe::GpuSwap(bits, fetch.endian);
    std::memcpy(&f, &swapped, 4);
    v[i] = int32_t(std::lround((f + half_pixel) * 256.0f));  // 16.8 fixed point
  }
  int32_t x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8, y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
  int32_t x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8, y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;
  const auto window_offset = regs.Get<rg::reg::PA_SC_WINDOW_OFFSET>();
  if (regs.Get<rg::reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    x0 += window_offset.window_x_offset, x1 += window_offset.window_x_offset;
    y0 += window_offset.window_y_offset, y1 += window_offset.window_y_offset;
  }
  const auto tl = regs.Get<rg::reg::PA_SC_WINDOW_SCISSOR_TL>();
  const auto br = regs.Get<rg::reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t sx0 = tl.tl_x, sy0 = tl.tl_y, sx1 = br.br_x, sy1 = br.br_y;
  if (!tl.window_offset_disable) {
    sx0 += window_offset.window_x_offset, sx1 += window_offset.window_x_offset;
    sy0 += window_offset.window_y_offset, sy1 += window_offset.window_y_offset;
  }
  x0 = std::clamp(x0, std::max(sx0, 0), std::max(sx1, 0)), x1 = std::clamp(x1, std::max(sx0, 0), std::max(sx1, 0));
  y0 = std::clamp(y0, std::max(sy0, 0), std::max(sy1, 0)), y1 = std::clamp(y1, std::max(sy0, 0), std::max(sy1, 0));
  x0 &= ~7, y0 &= ~7, x1 = (x1 + 7) & ~7, y1 = (y1 + 7) & ~7;  // D3D aligns resolves to 8
  const int32_t pitch = int32_t(regs.Get<rg::reg::RB_SURFACE_INFO>().surface_pitch & ~7u);
  x0 = std::min(x0, pitch), x1 = std::min(x1, pitch);
  if (x0 >= x1 || y0 >= y1) {
    ++stats_.dropped_resolves;
    return;
  }

  BeginCommands();
  EndRendering();
  uint32_t samples_x, samples_y;
  SampleGrid(regs, samples_x, samples_y);
  const bool multisampled = samples_x * samples_y > 1;
  Image* source = Surface(regs, depth_source, depth_source ? 0 : copy.copy_src_select);
  if (!source) {
    ++stats_.dropped_resolves;
    return;
  }
  auto clamp_rect = [&](const Image& image, int32_t& w, int32_t& h) {
    w = std::min(x1, int32_t(image.width)) - x0;
    h = std::min(y1, int32_t(image.height)) - y0;
    return w > 0 && h > 0;
  };

  // Copy.
  const auto dest_pitch = regs.Get<rg::reg::RB_COPY_DEST_PITCH>();
  const auto dest_info = regs.Get<rg::reg::RB_COPY_DEST_INFO>();
  const uint32_t dest_address = regs.values[rg::XE_GPU_REG_RB_COPY_DEST_BASE];
  const bool copying = (copy.copy_command == xe::CopyCommand::kRaw || copy.copy_command == xe::CopyCommand::kConvert) &&
                       dest_pitch.copy_dest_pitch && dest_pitch.copy_dest_height;
  if (trace_textures_) {
    const auto color_info = regs.Get<rg::reg::RB_COLOR_INFO>(
        rg::reg::RB_COLOR_INFO::rt_register_indices[depth_source ? 0 : copy.copy_src_select]);
    std::fprintf(stderr,
                 "resolve %llu: %s src %u (surface %ux%u fmt %u) rect %d,%d-%d,%d -> %08X fmt %u %ux%u cmd %u "
                 "bias %d num %u swap %u%s clear %s%s\n",
                 (unsigned long long)stats_.resolves, depth_source ? "depth" : "color", copy.copy_src_select,
                 source->width, source->height, depth_source ? 0u : uint32_t(color_info.color_format), x0, y0, x1, y1,
                 dest_address, uint32_t(dest_info.copy_dest_format), uint32_t(dest_pitch.copy_dest_pitch),
                 uint32_t(dest_pitch.copy_dest_height), uint32_t(copy.copy_command), int(dest_info.copy_dest_exp_bias),
                 uint32_t(dest_info.copy_dest_number), uint32_t(dest_info.copy_dest_swap), multisampled ? " msaa" : "", copy.color_clear_enable ? "color " : "", copy.depth_clear_enable ? "depth" : "");
  }
  if (copying && multisampled) {
    ++stats_.msaa_resolves;  // would need a downsample; none seen in this game yet
  } else if (copying && depth_source) {
    // D3D resolves depth to an 8888 texture holding the raw 24_8 bits; games sample it as 24_8 / 24_8_FLOAT
    // (the depth value) or as 8888 (to restore depth). Keep both.
    const bool float_depth =
        regs.Get<rg::reg::RB_DEPTH_INFO>().depth_format == xe::DepthRenderTargetFormat::kD24FS8;
    Image* raw = ResolveTexture(dest_address, uint32_t(xe::TextureFormat::k_8_8_8_8), dest_pitch.copy_dest_pitch,
                                dest_pitch.copy_dest_height);
    Image* value = ResolveTexture(dest_address, uint32_t(float_depth ? xe::TextureFormat::k_24_8_FLOAT
                                                                     : xe::TextureFormat::k_24_8),
                                  dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height, true);
    int32_t w, h, dw, dh;
    if (raw && value && clamp_rect(*source, w, h) && clamp_rect(*raw, dw, dh)) {
      const VkRect2D rect{{x0, y0}, {uint32_t(std::min(w, dw)), uint32_t(std::min(h, dh))}};
      Transfer(*source, float_depth ? kTransferFloatDepth : kTransferDepth, *raw,
               uint32_t(xe::ColorRenderTargetFormat::k_8_8_8_8), &rect);
      Transfer(*source, kTransferDepth, *value, kTransferDepthValue, &rect);
      raw->written = value->written = ++write_clock_;
      DumpResolve(*value, dest_address);
    } else {
      ++stats_.dropped_resolves;
    }
  } else if (copying) {
    const uint32_t format = uint32_t(dest_info.copy_dest_format);
    const auto number = dest_info.copy_dest_number;
    const bool is_signed = number == xe::SurfaceNumberFormat::kSignedRepeatingFraction ||
                           number == xe::SurfaceNumberFormat::kSignedInteger;
    Image* dest = ResolveTexture(dest_address, format, dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height,
                                 false, is_signed);
    int32_t w, h, dw, dh;
    if (dest && clamp_rect(*source, w, h) && clamp_rect(*dest, dw, dh)) {
      // The resolve converts through the shader into the destination's format and signedness, with the exponent
      // bias (UE3 copies its HDR scene out at 1/8). Fixed-point 16-bit surfaces hold value / 32 on the host. The
      // red/blue swap is not applied: the host image keeps red first and remembers the swap for sampling.
      const auto source_format = xe::GetStorageColorFormat(
          regs.Get<rg::reg::RB_COLOR_INFO>(rg::reg::RB_COLOR_INFO::rt_register_indices[copy.copy_src_select])
              .color_format);
      const bool fixed = source_format == xe::ColorRenderTargetFormat::k_16_16 ||
                         source_format == xe::ColorRenderTargetFormat::k_16_16_16_16;
      const VkRect2D rect{{x0, y0}, {uint32_t(std::min(w, dw)), uint32_t(std::min(h, dh))}};
      Transfer(*source, 0, *dest, kTransferResolve, &rect,
               std::ldexp(1.0f, int(dest_info.copy_dest_exp_bias) + (fixed ? 5 : 0)));
      dest->swapped = dest_info.copy_dest_swap;
      dest->written = ++write_clock_;
      last_resolve_address_ = dest_address;
      DumpResolve(*dest, dest_address);
    } else {
      ++stats_.dropped_resolves;
    }
  }

  // Clears, after the copy, of every sample in the rect.
  x0 *= samples_x, x1 *= samples_x, y0 *= samples_y, y1 *= samples_y;
  auto clear = [&](Image& image, bool depth, const VkClearValue& value) {
    int32_t w, h;
    if (!clamp_rect(image, w, h)) return;
    Barrier();
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = image.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {image.width, image.height}};
    rendering.layerCount = 1;
    if (depth) {
      rendering.pDepthAttachment = rendering.pStencilAttachment = &attachment;
    } else {
      rendering.colorAttachmentCount = 1;
      rendering.pColorAttachments = &attachment;
    }
    vk_.vkCmdBeginRendering(cmd_, &rendering);
    const VkClearAttachment what{depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                       : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                                 0, value};
    const VkClearRect rect{{{x0, y0}, {uint32_t(w), uint32_t(h)}}, 0, 1};
    vk_.vkCmdClearAttachments(cmd_, 1, &what, 1, &rect);
    vk_.vkCmdEndRendering(cmd_);
    image.written = ++write_clock_;
    ++stats_.clears;
  };
  if (copy.color_clear_enable && !depth_source) {
    const auto info =
        regs.Get<rg::reg::RB_COLOR_INFO>(rg::reg::RB_COLOR_INFO::rt_register_indices[copy.copy_src_select]);
    VkClearValue value;
    value.color = ColorClear(uint32_t(info.color_format), regs.values[rg::XE_GPU_REG_RB_COLOR_CLEAR],
                             regs.values[rg::XE_GPU_REG_RB_COLOR_CLEAR_LO]);
    clear(*source, false, value);
  }
  if (copy.depth_clear_enable) {
    if (Image* depth = Surface(regs, true, 0)) {
      const uint32_t raw = regs.values[rg::XE_GPU_REG_RB_DEPTH_CLEAR];
      const bool float_depth =
          regs.Get<rg::reg::RB_DEPTH_INFO>().depth_format == xe::DepthRenderTargetFormat::kD24FS8;
      VkClearValue value;
      value.depthStencil = {float_depth ? Float20e4(raw >> 8) : float(raw >> 8) / 16777215.0f, raw & 0xFF};
      clear(*depth, true, value);
    }
  }
}

// BOTS_DUMP_RESOLVES=<dir> writes every resolve as <dir>/resolve-<n>-<address>.ppm (debugging; depth resolves
// show the depth value).
void Renderer::DumpResolve(const Image& image, uint32_t address) {
  static const char* dump_dir = std::getenv("BOTS_DUMP_RESOLVES");
  if (!dump_dir || !*dump_dir) return;
  std::vector<uint8_t> rgb;
  uint32_t width, height;
  if (ReadImage(image, rgb, width, height)) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/resolve-%02llu-%08X.ppm", dump_dir, (unsigned long long)stats_.resolves,
                  address);
    if (FILE* f = std::fopen(path, "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", width, height);
      std::fwrite(rgb.data(), 1, rgb.size(), f);
      std::fclose(f);
    }
  }
  BeginCommands();
}

bool Renderer::ReadTexture(uint32_t address, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
  const auto it = texture_at_.find(address);
  return it != texture_at_.end() && ReadImage(*it->second, rgb, width, height);
}

bool Renderer::ReadImage(const Image& image, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
  uint32_t texel;
  switch (image.format) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UNORM:
      texel = 4;
      break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
      texel = 8;
      break;
    default:
      std::fprintf(stderr, "native renderer: no readback for format %d\n", int(image.format));
      return false;
  }
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = VkDeviceSize(image.width) * image.height * texel;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer buffer;
  VmaAllocation allocation;
  VmaAllocationInfo mapped;
  if (vmaCreateBuffer(allocator_, &info, &alloc, &buffer, &allocation, &mapped) != VK_SUCCESS) return false;

  BeginCommands();
  Barrier();
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {image.width, image.height, 1};
  vk_.vkCmdCopyImageToBuffer(cmd_, image.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
  Flush();
  vmaInvalidateAllocation(allocator_, allocation, 0, VK_WHOLE_SIZE);

  // BOTS_DUMP_SCALE=<f> scales the values first, to look at HDR targets (debugging).
  static const float scale = [] {
    const char* v = std::getenv("BOTS_DUMP_SCALE");
    return v ? float(std::atof(v)) : 1.0f;
  }();
  // BOTS_DUMP_7E3=1 shows 2_10_10_10 textures as 7e3 (UE3 copies HDR scene colour out through them).
  static const bool as_7e3 = std::getenv("BOTS_DUMP_7E3") != nullptr;
  width = image.width, height = image.height;
  rgb.resize(size_t(width) * height * 3);
  const auto* src = static_cast<const uint8_t*>(mapped.pMappedData);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    float c[3];
    if (image.format == VK_FORMAT_R32_SFLOAT) {
      float v;
      std::memcpy(&v, src + i * 4, 4);
      c[0] = c[1] = c[2] = v;
    } else if (image.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
      uint16_t h[4];
      std::memcpy(h, src + i * 8, 8);
      for (int k = 0; k < 3; ++k) c[k] = Half(h[k]);
    } else if (image.format == VK_FORMAT_R16G16B16A16_UNORM || image.format == VK_FORMAT_R16G16_UNORM) {
      const bool four = image.format == VK_FORMAT_R16G16B16A16_UNORM;
      uint16_t v[4] = {};
      std::memcpy(v, src + i * texel, four ? 8 : 4);
      for (int k = 0; k < 3; ++k) c[k] = (k < 2 || four) ? float(v[k]) / 65535.0f : 0.0f;
    } else if (image.format == VK_FORMAT_R16G16B16A16_SNORM || image.format == VK_FORMAT_R16G16_SNORM) {
      // Guest 16-bit fixed point: -32..32.
      const bool four = image.format == VK_FORMAT_R16G16B16A16_SNORM;
      int16_t v[4] = {};
      std::memcpy(v, src + i * texel, four ? 8 : 4);
      for (int k = 0; k < 3; ++k) c[k] = (k < 2 || four) ? float(v[k]) / 32767.0f * 32.0f : 0.0f;
    } else {
      uint32_t p;
      std::memcpy(&p, src + i * 4, 4);
      if (image.format == VK_FORMAT_R8G8B8A8_UNORM) {
        for (int k = 0; k < 3; ++k) c[k] = Unorm(p >> (8 * k), 8);
      } else if (as_7e3) {
        for (int k = 0; k < 3; ++k) c[k] = rex::graphics::xenos::Float7e3To32((p >> (10 * k)) & 0x3FF);
      } else {
        for (int k = 0; k < 3; ++k) c[k] = Unorm(p >> (10 * k), 10);
      }
    }
    for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = ToByte(c[k] * scale);
  }
  vmaDestroyBuffer(allocator_, buffer, allocation);
  return true;
}

}  // namespace bots::gfx::vk
