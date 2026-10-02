#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// Resolve destination (guest texture) format -> host format.
VkFormat TextureFormat(uint32_t format) {
  switch (xe::TextureFormat(format)) {
    case xe::TextureFormat::k_8:
      return VK_FORMAT_R8_UNORM;
    case xe::TextureFormat::k_8_8_8_8:
    case xe::TextureFormat::k_8_8_8_8_A:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xe::TextureFormat::k_2_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xe::TextureFormat::k_10_11_11:
    case xe::TextureFormat::k_11_11_10:
      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case xe::TextureFormat::k_16_16:
      return VK_FORMAT_R16G16_SNORM;
    case xe::TextureFormat::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SNORM;
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
    if (image.view) vk_.vkDestroyImageView(dev, image.view, nullptr);
    if (image.image) vmaDestroyImage(allocator_, image.image, image.allocation);
  };
  for (auto& [k, image] : surfaces_) destroy(*image);
  for (auto& [k, image] : textures_) destroy(*image);
  for (Image* image : {&null_2d_array_, &null_3d_, &null_cube_}) destroy(*image);
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
                           VkImageViewType view_type) {
  const bool cube = view_type == VK_IMAGE_VIEW_TYPE_CUBE;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = cube ? 6 : 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
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
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = view_type;
  view.format = format;
  view.subresourceRange = {image.aspect, 0, 1, 0, info.arrayLayers};
  if (vk_.vkCreateImageView(device_->device(), &view, nullptr, &image.view) != VK_SUCCESS) return false;

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
    base = info.depth_base, format = uint32_t(info.depth_format);
  } else {
    const auto info = regs.Get<rg::reg::RB_COLOR_INFO>(rg::reg::RB_COLOR_INFO::rt_register_indices[color_index]);
    base = info.color_base, format = uint32_t(info.color_format);
    is_64bpp = xe::IsColorRenderTargetFormat64bpp(info.color_format);
  }
  const uint32_t pitch = surface.surface_pitch;
  if (!pitch) return nullptr;
  const SurfaceKey key{base, depth, format, pitch};
  auto& slot = surfaces_[key];
  if (slot) return slot.get();

  // No height on Xenos: the surface can extend to the end of EDRAM.
  const uint32_t samples_x = surface.msaa_samples >= xe::MsaaSamples::k4X ? 2 : 1;
  const uint32_t samples_y = surface.msaa_samples >= xe::MsaaSamples::k2X ? 2 : 1;
  const uint32_t pitch_tiles = ((pitch * samples_x + kTileWidthSamples - 1) / kTileWidthSamples) << is_64bpp;
  const uint32_t rows = std::max(1u, (kEdramTiles - base % kEdramTiles) / pitch_tiles);
  const uint32_t height = std::min(rows * kTileHeightSamples / samples_y, kMaxSurfaceHeight);
  auto image = std::make_unique<Image>();
  if (!CreateImage(*image, depth ? kDepthFormat : ColorTargetFormat(format), pitch, height, depth)) return nullptr;
  slot = std::move(image);
  return slot.get();
}

Renderer::Image* Renderer::ResolveTexture(uint32_t address, uint32_t format, uint32_t width, uint32_t height) {
  const TextureKey key{address, format, width, height};
  auto& slot = textures_[key];
  if (!slot) {
    auto image = std::make_unique<Image>();
    // Shaders sample 2D textures as arrays.
    if (!CreateImage(*image, TextureFormat(format), width, height, false, VK_IMAGE_VIEW_TYPE_2D_ARRAY)) return nullptr;
    slot = std::move(image);
  }
  texture_at_[address] = slot.get();
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
  if (copying && !depth_source) {  // depth resolves (to 24_8 textures) need a shader: M6
    const uint32_t format = uint32_t(dest_info.copy_dest_format);
    Image* dest = ResolveTexture(dest_address, format, dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height);
    int32_t w, h, dw, dh;
    if (dest && clamp_rect(*source, w, h) && clamp_rect(*dest, dw, dh)) {
      w = std::min(w, dw), h = std::min(h, dh);
      Barrier();
      VkImageBlit blit{};
      blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[0] = blit.dstOffsets[0] = {x0, y0, 0};
      blit.srcOffsets[1] = blit.dstOffsets[1] = {x0 + w, y0 + h, 1};
      vk_.vkCmdBlitImage(cmd_, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image, VK_IMAGE_LAYOUT_GENERAL, 1,
                         &blit, VK_FILTER_NEAREST);
      last_resolve_address_ = dest_address;
    } else {
      ++stats_.dropped_resolves;
    }
  } else if (copying) {
    ++stats_.dropped_resolves;
  }

  // Clears, after the copy.
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

bool Renderer::ReadTexture(uint32_t address, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
  const auto it = texture_at_.find(address);
  if (it == texture_at_.end()) return false;
  const Image& image = *it->second;
  uint32_t texel;
  switch (image.format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      texel = 4;
      break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
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

  width = image.width, height = image.height;
  rgb.resize(size_t(width) * height * 3);
  const auto* src = static_cast<const uint8_t*>(mapped.pMappedData);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    float c[3];
    if (image.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
      uint16_t h[4];
      std::memcpy(h, src + i * 8, 8);
      for (int k = 0; k < 3; ++k) c[k] = Half(h[k]);
    } else {
      uint32_t p;
      std::memcpy(&p, src + i * 4, 4);
      if (image.format == VK_FORMAT_R8G8B8A8_UNORM) {
        for (int k = 0; k < 3; ++k) c[k] = Unorm(p >> (8 * k), 8);
      } else {
        for (int k = 0; k < 3; ++k) c[k] = Unorm(p >> (10 * k), 10);
      }
    }
    for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = ToByte(c[k]);
  }
  vmaDestroyBuffer(allocator_, buffer, allocation);
  return true;
}

}  // namespace bots::gfx::vk
