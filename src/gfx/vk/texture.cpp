// Renderer: guest textures (loaded on the CPU: untile + byte swap + a few bit shuffles) and samplers.
//
// 1D, 2D, stacked, cube and 3D textures with all their mip levels; formats without a host equivalent bind the
// null image. Resolve destinations (render-to-texture) are sampled from their host images instead.
// Textures are cached by the fetch constant fields that describe their memory. With a memory tracker (live) the
// memory they came from is watched, and a texture the CPU wrote over is reloaded on its next use.
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
  // The fields that describe the memory: pitch/tiled, format/endian/stacked/address, size, mip range,
  // dimension/packing/mip address.
  const TextureCacheKey key{fetch.dword_0 & 0xFFC00000u, fetch.dword_1, fetch.dword_2, (fetch.dword_4 >> 2) & 0xFFu,
                            fetch.dword_5 & 0xFFFFFE00u};
  auto& slot = guest_textures_[key];
  if (!slot) {
    slot = std::make_unique<GuestTexture>();
    LoadTexture(fetch, *slot);
  } else if (slot->dirty) {
    Retire(slot->image);  // earlier draws of this submission still sample the old contents
    *slot = GuestTexture{};
    LoadTexture(fetch, *slot);
    ++stats_.texture_reloads;
  }
  GuestTexture& texture = *slot;
  texture.last_used = frame_;
  if (!texture.image.image) return VK_NULL_HANDLE;
  host_swizzle = HostSwizzle(fetch.swizzle, texture.host_format_swizzle);
  return SwizzledView(texture.image, host_swizzle, is_signed);
}

Renderer::Image* Renderer::ResolvedTexture(const rg::xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  Image* image = nullptr;
  if (fetch.format == xe::TextureFormat::k_24_8 || fetch.format == xe::TextureFormat::k_24_8_FLOAT) {
    if (const auto it = depth_texture_at_.find(address); it != depth_texture_at_.end()) image = it->second;
  }
  if (!image) {
    const auto it = texture_at_.find(address);
    if (it == texture_at_.end()) return nullptr;
    image = it->second;
  }
  // The fetch's size, not the resolve's pitch, is what normalized coordinates span.
  const uint32_t width = std::min(uint32_t(fetch.size_2d.width) + 1, image->width);
  const uint32_t height = std::min(uint32_t(fetch.size_2d.height) + 1, image->height);
  if (image->raw_depth_from) MaterializeRawDepth(*image);
  if (width == image->width && height == image->height) return image;
  return Crop(*image, width, height);
}

Renderer::Image* Renderer::Crop(Image& image, uint32_t width, uint32_t height) {
  auto& slot = crops_[{&image, width, height}];
  if (!slot) {
    auto crop = std::make_unique<Image>();
    if (!CreateImage(*crop, image.format, width, height, false, VK_IMAGE_VIEW_TYPE_2D_ARRAY)) return nullptr;
    slot = std::move(crop);
  }
  Image& crop = *slot;
  if (crop.written != image.written) {
    BeginCommands(kGpuTexture);
    EndRendering("texture load");
    Image* const read = &image;
    Image* const write = &crop;
    Access({&read, 1}, {&write, 1});
    const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             {0, 0, 0}, {width, height, 1}};
    vk_.vkCmdCopyImage(cmd_, image.image, VK_IMAGE_LAYOUT_GENERAL, crop.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    crop.written = image.written;
  }
  crop.swapped = image.swapped;
  return &crop;
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
    info.viewType = image.view_type;
    info.format = other ? twin : image.format;
    info.components = {ComponentSwizzle(swizzle & 7), ComponentSwizzle((swizzle >> 3) & 7),
                       ComponentSwizzle((swizzle >> 6) & 7), ComponentSwizzle((swizzle >> 9) & 7)};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.levels, 0, image.layers};
    if (vk_.vkCreateImageView(device_->device(), &info, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
  }
  return view;
}

void Renderer::LoadTexture(const rg::xenos::xe_gpu_texture_fetch_t& fetch, GuestTexture& texture) {
  ScopedMicros time(stats_.texture_us);
  uint32_t width_m1, height_m1, depth_m1, base_page, mip_page, mip_min, mip_max;
  rg::texture_util::GetSubresourcesFromFetchConstant(fetch, &width_m1, &height_m1, &depth_m1, &base_page, &mip_page,
                                                     &mip_min, &mip_max);
  const HostFormat host = GetHostFormat(fetch.format);
  if (host.format == VK_FORMAT_UNDEFINED || (!base_page && !mip_page)) {
    ++stats_.unsupported_textures;
    if (trace_textures_) std::fprintf(stderr, "texture: unsupported format %u\n", uint32_t(fetch.format));
    return;
  }
  const xe::DataDimension dimension = fetch.dimension;
  const bool is_3d = dimension == xe::DataDimension::k3D;
  const bool cube = dimension == xe::DataDimension::kCube;
  const uint32_t width = width_m1 + 1, height = height_m1 + 1, depth = depth_m1 + 1;
  const uint32_t layers = is_3d ? 1 : depth;
  const rg::FormatInfo* format_info = rg::FormatInfo::Get(uint32_t(fetch.format));
  const uint32_t bpb = format_info->bytes_per_block();
  uint32_t bpb_log2 = 0;
  while ((2u << bpb_log2) <= bpb) ++bpb_log2;
  const auto layout = rg::texture_util::GetGuestTextureLayout(dimension, fetch.pitch, width, height, depth, fetch.tiled,
                                                             fetch.format, fetch.packed_mips, base_page != 0, mip_max);
  const VkImageViewType view_type = is_3d ? VK_IMAGE_VIEW_TYPE_3D
                                    : cube ? VK_IMAGE_VIEW_TYPE_CUBE
                                           : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  if (!CreateImage(texture.image, host.format, width, height, false, view_type, false, depth, mip_max + 1)) {
    std::fprintf(stderr, "native renderer: texture image %ux%ux%u format %d failed\n", width, height, depth,
                 int(host.format));
    texture.image = {};
    ++stats_.unsupported_textures;
    return;
  }
  texture.host_format_swizzle = host.swizzle;
  if (base_page) texture.ranges[0] = {base_page << 12, (base_page << 12) + layout.base.level_data_extent_bytes};
  if (mip_page && mip_max) texture.ranges[1] = {mip_page << 12, (mip_page << 12) + layout.mips_total_extent_bytes};
  if (tracker_) {
    // Requesting the pages is what puts a write watch on them (and keeps the shared-memory copy right where a
    // page is shared with a buffer).
    for (const auto& [start, end] : texture.ranges) {
      if (end > start) tracker_->Request(start, end - start);
    }
  }

  // Levels below mip_min stay undefined: the sampler's LOD range never reaches them.
  for (uint32_t level = mip_min; level <= mip_max; ++level) {
    const bool base = level == 0;
    const uint32_t stored = std::min(level, layout.packed_level);
    const auto& guest = base ? layout.base : layout.mips[stored];
    const uint32_t level_address = base ? base_page << 12 : (mip_page << 12) + layout.mip_offsets_bytes[stored];
    uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
    if (level >= layout.packed_level) {
      rg::texture_util::GetPackedMipOffset(width, height, is_3d ? depth : 1, fetch.format, level, offset_x, offset_y,
                                           offset_z);
    }
    const uint32_t level_width = std::max(width >> level, 1u), level_height = std::max(height >> level, 1u);
    const uint32_t level_depth = is_3d ? std::max(depth >> level, 1u) : 1;
    const uint32_t blocks_x = (level_width + format_info->block_width - 1) / format_info->block_width;
    const uint32_t blocks_y = (level_height + format_info->block_height - 1) / format_info->block_height;
    const uint32_t pitch_blocks = guest.row_pitch_bytes / bpb;
    const VkDeviceSize bytes = VkDeviceSize(blocks_x) * blocks_y * level_depth * layers * bpb;
    VkDeviceSize offset;
    uint8_t* staging = Upload(bytes, 16, offset);
    if (!staging) {
      Flush();
      BeginCommands(kGpuTexture);
      staging = Upload(bytes, 16, offset);
      if (!staging) {
        ++stats_.unsupported_textures;
        return;
      }
    }
    // Block by block through the SDK's tiled-address functions: slow but exact for every layout (P5: GPU untile).
    uint8_t* out = staging;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      const uint8_t* source = memory_(level_address + layer * guest.array_slice_stride_bytes);
      for (uint32_t z = 0; z < level_depth; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          for (uint32_t x = 0; x < blocks_x; ++x, out += bpb) {
            const int32_t bx = int32_t(x + offset_x), by = int32_t(y + offset_y), bz = int32_t(z + offset_z);
            int32_t at;
            if (!fetch.tiled) {
              at = (bz * int32_t(guest.z_slice_stride_block_rows) + by) * int32_t(guest.row_pitch_bytes) + bx * int32_t(bpb);
            } else if (is_3d) {
              at = rg::texture_util::GetTiledOffset3D(bx, by, bz, pitch_blocks, guest.z_slice_stride_block_rows,
                                                      bpb_log2);
            } else {
              at = rg::texture_util::GetTiledOffset2D(bx, by, pitch_blocks, bpb_log2);
            }
            rg::texture_conversion::CopySwapBlock(fetch.endianness, out, source + at, bpb);
          }
        }
      }
    }
    ApplyShuffle(host.shuffle, staging, bytes);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.bufferRowLength = blocks_x * format_info->block_width;
    region.bufferImageHeight = blocks_y * format_info->block_height;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, texture.image.layers};
    region.imageExtent = {level_width, level_height, level_depth};
    vk_.vkCmdCopyBufferToImage(cmd_, ring_.buffer, texture.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  }
  Barrier();
  ++stats_.textures;
  if (trace_textures_) {
    std::fprintf(stderr, "texture %08X: %ux%ux%u dim %u levels %u-%u format %u%s endian %u -> vk %d\n", base_page << 12,
                 width, height, depth, uint32_t(dimension), mip_min, mip_max, uint32_t(fetch.format),
                 fetch.tiled ? " tiled" : "", uint32_t(fetch.endianness), int(host.format));
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
  xe::AnisoFilter aniso =
      binding.aniso_filter == xe::AnisoFilter::kUseFetchConst ? fetch.aniso_filter : binding.aniso_filter;
  // Host max anisotropy as an AnisoFilter (kMax_1_1 = 1x ... kMax_16_1 = 16x).
  const auto& props = device_->properties();
  uint32_t host_max = uint32_t(xe::AnisoFilter::kDisabled);
  if (props.samplerAnisotropy) {
    for (uint32_t n = 1; n <= std::min(16.0f, props.maxSamplerAnisotropy); n <<= 1) ++host_max;
  }
  if (aniso == xe::AnisoFilter::kUseFetchConst) aniso = xe::AnisoFilter::kDisabled;
  aniso = xe::AnisoFilter(std::min(uint32_t(aniso), host_max));
  const SamplerKey key{uint32_t(mag),           uint32_t(min),          uint32_t(mip),
                       uint32_t(fetch.clamp_x),  uint32_t(fetch.clamp_y), uint32_t(fetch.clamp_z),
                       uint32_t(fetch.border_color), fetch.mip_min_level, fetch.mip_max_level, uint32_t(aniso)};
  VkSampler& sampler = samplers_[key];
  if (sampler) return sampler;
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = mag == xe::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = min == xe::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = mip == xe::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = AddressMode(fetch.clamp_x);
  info.addressModeV = AddressMode(fetch.clamp_y);
  info.addressModeW = AddressMode(fetch.clamp_z);
  // Host level n is guest level n, so the fetch's mip range is the LOD range.
  info.minLod = float(fetch.mip_min_level);
  info.maxLod = mip == xe::TextureFilter::kBaseMap ? info.minLod + 0.25f
                                                   : float(std::max(fetch.mip_min_level, fetch.mip_max_level));
  if (aniso != xe::AnisoFilter::kDisabled) {
    // Like the SDK (and D3D): anisotropic filtering forces linear min/mag/mip.
    info.magFilter = info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = float(1u << (uint32_t(aniso) - uint32_t(xe::AnisoFilter::kMax_1_1)));
  }
  switch (fetch.border_color) {
    case xe::BorderColor::k_ABGR_White: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
    case xe::BorderColor::k_ABGR_Black: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
    default: info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
  }
  if (vk_.vkCreateSampler(device_->device(), &info, nullptr, &sampler) != VK_SUCCESS) sampler = sampler_;
  return sampler;
}

}  // namespace bots::gfx::vk
