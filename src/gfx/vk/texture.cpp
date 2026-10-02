// Renderer: guest textures (loaded on the CPU: untile + byte swap + a few bit shuffles) and samplers.
//
// Only the base level of 2D textures is loaded so far; mips, stacked, 3D and cube textures bind the null image.
// Resolve destinations (render-to-texture) are sampled from their host images instead.
// Textures are cached by the fetch constant fields that describe their memory, and never invalidated: fine for
// replays (static memory), not for the game (P3 needs write tracking).
#include <algorithm>
#include <cstdio>
#include <cstring>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>

#include "renderer.h"

namespace bots::gfx::vk {
namespace {

namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

enum class Shuffle { kNone, kSwapRB565, kSwapRB1555, kRotate4444 };

struct HostFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  // Where the guest's R, G, B, A come from in the host format (XE_GPU_TEXTURE_SWIZZLE components).
  uint32_t swizzle = xe::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  Shuffle shuffle = Shuffle::kNone;
};

// After the endian swap the guest texel's first component is in the lowest bits, like Vulkan's R-first formats
// (the SDK's Vulkan texture cache uses the same host formats; its load shaders do what Shuffle does here).
HostFormat GetHostFormat(xe::TextureFormat format) {
  using F = xe::TextureFormat;
  constexpr uint32_t kRRRR = xe::XE_GPU_TEXTURE_SWIZZLE_RRRR, kRGGG = xe::XE_GPU_TEXTURE_SWIZZLE_RGGG,
                     kRGBB = xe::XE_GPU_TEXTURE_SWIZZLE_RGBB, kRGBA = xe::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  switch (rg::GetBaseFormat(format)) {
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B: return {VK_FORMAT_R8_UNORM, kRRRR};
    case F::k_1_5_5_5: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, kRGBA, Shuffle::kSwapRB1555};
    case F::k_5_6_5: return {VK_FORMAT_R5G6B5_UNORM_PACK16, kRGBB, Shuffle::kSwapRB565};
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A: return {VK_FORMAT_R8G8B8A8_UNORM, kRGBA};
    case F::k_2_10_10_10: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, kRGBA};
    case F::k_8_8: return {VK_FORMAT_R8G8_UNORM, kRGGG};
    case F::k_4_4_4_4: return {VK_FORMAT_B4G4R4A4_UNORM_PACK16, kRGBA, Shuffle::kRotate4444};
    case F::k_DXT1: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, kRGBA};
    case F::k_DXT2_3: return {VK_FORMAT_BC2_UNORM_BLOCK, kRGBA};
    case F::k_DXT4_5: return {VK_FORMAT_BC3_UNORM_BLOCK, kRGBA};
    case F::k_DXN: return {VK_FORMAT_BC5_UNORM_BLOCK, kRGGG};
    case F::k_DXT5A: return {VK_FORMAT_BC4_UNORM_BLOCK, kRRRR};
    case F::k_16: return {VK_FORMAT_R16_UNORM, kRRRR};
    case F::k_16_16: return {VK_FORMAT_R16G16_UNORM, kRGGG};
    case F::k_16_16_16_16: return {VK_FORMAT_R16G16B16A16_UNORM, kRGBA};
    case F::k_16_FLOAT: return {VK_FORMAT_R16_SFLOAT, kRRRR};
    case F::k_16_16_FLOAT: return {VK_FORMAT_R16G16_SFLOAT, kRGGG};
    case F::k_16_16_16_16_FLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, kRGBA};
    case F::k_32_FLOAT: return {VK_FORMAT_R32_SFLOAT, kRRRR};
    case F::k_32_32_FLOAT: return {VK_FORMAT_R32G32_SFLOAT, kRGGG};
    case F::k_32_32_32_32_FLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, kRGBA};
    default: return {};
  }
}

void ApplyShuffle(Shuffle shuffle, uint8_t* data, size_t bytes) {
  if (shuffle == Shuffle::kNone) return;
  for (size_t i = 0; i + 2 <= bytes; i += 2) {
    uint16_t v;
    std::memcpy(&v, data + i, 2);
    switch (shuffle) {
      case Shuffle::kSwapRB565:
        v = uint16_t((v & 0x07E0) | (v >> 11) | (v << 11));
        break;
      case Shuffle::kSwapRB1555:
        v = uint16_t((v & 0x83E0) | ((v >> 10) & 0x1F) | ((v & 0x1F) << 10));
        break;
      case Shuffle::kRotate4444:
        v = uint16_t((v << 4) | (v >> 12));
        break;
      default: break;
    }
    std::memcpy(data + i, &v, 2);
  }
}

// Guest swizzle (fetch constant) through the host format's swizzle; constants 0/1 pass through.
uint32_t HostSwizzle(uint32_t guest, uint32_t host_format) {
  uint32_t result = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t g = (guest >> (3 * i)) & 7;
    const uint32_t h = g >= xe::XE_GPU_TEXTURE_SWIZZLE_0 ? (g & 0b101) : (host_format >> (3 * g)) & 7;
    result |= h << (3 * i);
  }
  return result;
}

VkComponentSwizzle ComponentSwizzle(uint32_t component) {
  switch (component) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
  }
}

VkSamplerAddressMode AddressMode(xe::ClampMode mode) {
  switch (mode) {
    case xe::ClampMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case xe::ClampMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case xe::ClampMode::kClampToBorder:
    case xe::ClampMode::kMirrorClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // halfway and mirror-once approximated
  }
}

}  // namespace

VkFormat Renderer::SignTwin(VkFormat format, bool* is_signed) {
  static constexpr std::pair<VkFormat, VkFormat> kTwins[] = {
      {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM},
      {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM},
      {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM},
      {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM},
      {VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM},
      {VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM},
      {VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK},
      {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK},
  };
  for (const auto& [unorm, snorm] : kTwins) {
    if (format == unorm || format == snorm) {
      if (is_signed) *is_signed = format == snorm;
      return format == unorm ? snorm : unorm;
    }
  }
  return VK_FORMAT_UNDEFINED;
}

VkImageView Renderer::TextureView(const rg::xenos::xe_gpu_texture_fetch_t& fetch, bool is_signed,
                                  uint32_t& host_swizzle) {
  // The fields that describe the memory: pitch/tiled, format/endian/stacked/address, size, dimension/packing.
  const TextureCacheKey key{fetch.dword_0 & 0xFFC00000u, fetch.dword_1, fetch.dword_2, fetch.dword_5 & 0x00000E00u};
  auto& slot = guest_textures_[key];
  if (!slot) {
    slot = std::make_unique<GuestTexture>();
    LoadTexture(fetch, *slot);
  }
  GuestTexture& texture = *slot;
  if (!texture.image.image) return VK_NULL_HANDLE;
  host_swizzle = HostSwizzle(fetch.swizzle, texture.host_format_swizzle);
  return SwizzledView(texture.image, host_swizzle, is_signed);
}

Renderer::Image* Renderer::ResolvedTexture(const rg::xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (fetch.format == xe::TextureFormat::k_24_8 || fetch.format == xe::TextureFormat::k_24_8_FLOAT) {
    if (const auto it = depth_texture_at_.find(address); it != depth_texture_at_.end()) return it->second;
  }
  const auto it = texture_at_.find(address);
  return it != texture_at_.end() ? it->second : nullptr;
}

VkImageView Renderer::ResolvedTextureView(Image& image, const rg::xenos::xe_gpu_texture_fetch_t& fetch,
                                          bool is_signed, uint32_t& host_swizzle) {
  // Channels the host format lacks read like the guest's (1- and 2-component formats repeat their last one).
  uint32_t host_format = xe::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  switch (image.format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R32_SFLOAT: host_format = xe::XE_GPU_TEXTURE_SWIZZLE_RRRR; break;
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: host_format = xe::XE_GPU_TEXTURE_SWIZZLE_RGGG; break;
    default: break;
  }
  // After a swapping resolve the guest's first component is blue; the host image keeps red first.
  if (image.swapped) host_format = 2u | (1u << 3) | (0u << 6) | (3u << 9);
  host_swizzle = HostSwizzle(fetch.swizzle, host_format);
  return SwizzledView(image, host_swizzle, is_signed);
}

VkImageView Renderer::SwizzledView(Image& image, uint32_t host_swizzle, bool is_signed) {
  bool image_signed = false;
  const VkFormat twin = SignTwin(image.format, &image_signed);
  const bool other = twin != VK_FORMAT_UNDEFINED && is_signed != image_signed;
  // Without view swizzles the shader swizzles (the translator reads texture_swizzles).
  const uint32_t swizzle =
      device_->properties().imageViewFormatSwizzle ? host_swizzle : uint32_t(xe::XE_GPU_TEXTURE_SWIZZLE_RGBA);
  if (!other && swizzle == xe::XE_GPU_TEXTURE_SWIZZLE_RGBA) return image.view;
  VkImageView& view = image.swizzled_views[swizzle | uint32_t(other) << 12];
  if (!view) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    info.format = other ? twin : image.format;
    info.components = {ComponentSwizzle(swizzle & 7), ComponentSwizzle((swizzle >> 3) & 7),
                       ComponentSwizzle((swizzle >> 6) & 7), ComponentSwizzle((swizzle >> 9) & 7)};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk_.vkCreateImageView(device_->device(), &info, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
  }
  return view;
}

void Renderer::LoadTexture(const rg::xenos::xe_gpu_texture_fetch_t& fetch, GuestTexture& texture) {
  rg::TextureInfo info;
  if (fetch.dimension != xe::DataDimension::k2DOrStacked || fetch.stacked ||
      !rg::TextureInfo::Prepare(fetch, &info)) {
    ++stats_.unsupported_textures;
    return;
  }
  const HostFormat host = GetHostFormat(info.format);
  if (host.format == VK_FORMAT_UNDEFINED) {
    ++stats_.unsupported_textures;
    if (trace_textures_) std::fprintf(stderr, "texture: unsupported format %u\n", uint32_t(info.format));
    return;
  }
  const rg::FormatInfo* format_info = info.format_info();
  const uint32_t block_bytes = format_info->bytes_per_block();
  uint32_t offset_x, offset_y;
  const uint32_t address = info.GetMipLocation(0, &offset_x, &offset_y, true);
  const uint8_t* source = memory_(address);
  if (!source) {
    ++stats_.unsupported_textures;
    return;
  }
  const rg::TextureExtent guest = info.GetMipExtent(0, true);
  const rg::TextureExtent host_extent = info.GetMipExtent(0, false);
  const uint32_t blocks_x = host_extent.block_width, blocks_y = host_extent.block_height;
  const VkDeviceSize bytes = VkDeviceSize(blocks_x) * blocks_y * block_bytes;
  VkDeviceSize offset;
  uint8_t* staging = Upload(bytes, 16, offset);
  if (!staging) {
    Flush();
    BeginCommands();
    staging = Upload(bytes, 16, offset);
    if (!staging) {
      ++stats_.unsupported_textures;
      return;
    }
  }
  const xe::Endian endian = info.endianness;
  if (info.is_tiled) {
    rg::texture_conversion::UntileInfo untile{};
    untile.offset_x = offset_x;
    untile.offset_y = offset_y;
    untile.width = blocks_x;
    untile.height = blocks_y;
    untile.input_pitch = guest.block_pitch_h;
    untile.output_pitch = blocks_x;
    untile.input_format_info = untile.output_format_info = format_info;
    untile.copy_callback = [endian](void* out, const void* in, size_t length) {
      rg::texture_conversion::CopySwapBlock(endian, out, in, length);
    };
    rg::texture_conversion::Untile(staging, source, &untile);
  } else {
    for (uint32_t y = 0; y < blocks_y; ++y) {
      rg::texture_conversion::CopySwapBlock(
          endian, staging + size_t(y) * blocks_x * block_bytes,
          source + (size_t(offset_y + y) * guest.block_pitch_h + offset_x) * block_bytes, size_t(blocks_x) * block_bytes);
    }
  }
  ApplyShuffle(host.shuffle, staging, bytes);

  const uint32_t width = info.width + 1, height = info.height + 1;
  if (!CreateImage(texture.image, host.format, width, height, false, VK_IMAGE_VIEW_TYPE_2D_ARRAY, false)) {
    std::fprintf(stderr, "native renderer: texture image %ux%u format %d failed\n", width, height, int(host.format));
    texture.image = {};
    ++stats_.unsupported_textures;
    return;
  }
  texture.host_format_swizzle = host.swizzle;
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.bufferRowLength = blocks_x * format_info->block_width;
  region.bufferImageHeight = blocks_y * format_info->block_height;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  vk_.vkCmdCopyBufferToImage(cmd_, ring_.buffer, texture.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  Barrier();
  ++stats_.textures;
  if (trace_textures_) {
    std::fprintf(stderr, "texture %08X: %ux%u format %u%s endian %u -> vk %d\n", address, width, height,
                 uint32_t(info.format), info.is_tiled ? " tiled" : "", uint32_t(endian), int(host.format));
  }
}

VkSampler Renderer::GetSampler(const rg::xenos::xe_gpu_texture_fetch_t& fetch,
                               const rg::SpirvShader::SamplerBinding& binding) {
  auto pick = [](xe::TextureFilter instruction, xe::TextureFilter constant) {
    return instruction == xe::TextureFilter::kUseFetchConst ? constant : instruction;
  };
  const xe::TextureFilter mag = pick(binding.mag_filter, fetch.mag_filter);
  const xe::TextureFilter min = pick(binding.min_filter, fetch.min_filter);
  const xe::TextureFilter mip = pick(binding.mip_filter, fetch.mip_filter);
  const SamplerKey key{uint32_t(mag), uint32_t(min), uint32_t(mip), uint32_t(fetch.clamp_x), uint32_t(fetch.clamp_y),
                       uint32_t(fetch.clamp_z), uint32_t(fetch.border_color)};
  VkSampler& sampler = samplers_[key];
  if (sampler) return sampler;
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = mag == xe::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = min == xe::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = mip == xe::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = AddressMode(fetch.clamp_x);
  info.addressModeV = AddressMode(fetch.clamp_y);
  info.addressModeW = AddressMode(fetch.clamp_z);
  info.maxLod = mip == xe::TextureFilter::kBaseMap ? 0.25f : VK_LOD_CLAMP_NONE;
  switch (fetch.border_color) {
    case xe::BorderColor::k_ABGR_White: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
    case xe::BorderColor::k_ABGR_Black: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
    default: info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
  }
  if (vk_.vkCreateSampler(device_->device(), &info, nullptr, &sampler) != VK_SUCCESS) sampler = sampler_;
  return sampler;
}

}  // namespace bots::gfx::vk
