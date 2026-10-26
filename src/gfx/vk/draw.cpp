// Renderer: guest draws (shader translation, pipelines, constants, indices).
//
// Follows the SDK's VulkanCommandProcessor::IssueDraw (host render target path) for everything the translated
// shaders depend on - modifications, system constants, constant buffer packing, descriptor layout - and does
// its own thing for render targets (renderer.h) and indices (built on the CPU, already byte-swapped).
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "renderer.h"
#include "draw_dump.h"
#include <string>

namespace bots::gfx::vk {
namespace {

namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace du = rex::graphics::draw_util;
using Translator = rg::SpirvShaderTranslator;

constexpr VkDeviceSize kSharedMemorySize = VkDeviceSize(512) << 20;
// Per submission (kSubmissions of them, host-visible). Gameplay uses at most ~14 MB of one; loads fill it and flush
// early, which costs nothing measurable (B-113: was 128 MB, 768 MB in all). The Apple TV's per-app memory limit
// (about 2.2 GB on 4 GB models, with no entitlement to raise it) is tight enough that its rings are halved.
#if defined(TARGET_OS_TV) && TARGET_OS_TV
constexpr VkDeviceSize kRingSize = VkDeviceSize(16) << 20;
#else
constexpr VkDeviceSize kRingSize = VkDeviceSize(32) << 20;
#endif
// Constant buffers are dynamic uniform buffers over the ring with a fixed range each (one set per ring, only the
// offsets change per draw). Float constants are packed to what the shader uses, at most 256 registers; the ring
// has this much slack past kRingSize so a range starting near its end stays inside the buffer.
constexpr VkDeviceSize kFloatConstantsRange = 256 * 16;
constexpr VkDeviceSize kRingSlack = kFloatConstantsRange;
// By Translator::ConstantBuffer: system, float vertex, float pixel, bool/loop, fetch (as IssueDraw uploads them).
constexpr VkDeviceSize kConstantRanges[Translator::kConstantBufferCount] = {
    sizeof(Translator::SystemConstants), kFloatConstantsRange, kFloatConstantsRange, 4 * (8 + 32), 4 * 6 * 32};
constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

VkBlendFactor BlendFactor(xe::BlendFactor f) {
  switch (uint32_t(f)) {
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ZERO;
  }
}

VkBlendOp BlendOp(xe::BlendOp op) {
  switch (op) {
    case xe::BlendOp::kSubtract: return VK_BLEND_OP_SUBTRACT;
    case xe::BlendOp::kMin: return VK_BLEND_OP_MIN;
    case xe::BlendOp::kMax: return VK_BLEND_OP_MAX;
    case xe::BlendOp::kRevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

// Xenos compare functions and stencil ops are in Vulkan's order.
VkCompareOp CompareOp(xe::CompareFunction f) { return VkCompareOp(uint32_t(f)); }
VkStencilOp StencilOp(xe::StencilOp op) { return VkStencilOp(uint32_t(op)); }

// 16-bit indices only swap within 16 bits (as the SDK normalizes it).
xe::Endian IndexEndian(xe::Endian endian, bool index32) {
  if (index32 || endian == xe::Endian::kNone || endian == xe::Endian::k8in16) return endian;
  return endian == xe::Endian::k8in32 ? xe::Endian::k8in16 : xe::Endian::kNone;
}

// Guest DMA indices -> host index buffer, byte-swapped, the guest reset index mapped to all ones (Vulkan's restart
// value). One pass that only writes dst: the ring is write-combined host memory, where reading back is very slow.
// This was a third of the draw-recording time as a per-index loop through a std::vector.
template <typename T, xe::Endian kEndian, bool kReset>
bool ConvertIndicesAs(const uint8_t* src, T* dst, uint32_t count, uint32_t reset_index) {
  bool any = false;
  for (uint32_t i = 0; i < count; ++i) {
    T v;
    std::memcpy(&v, src + size_t(i) * sizeof(T), sizeof(T));
    if constexpr (sizeof(T) == 2) {
      if constexpr (kEndian == xe::Endian::k8in16) v = __builtin_bswap16(v);
    } else if constexpr (kEndian == xe::Endian::k8in16) {
      v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    } else if constexpr (kEndian == xe::Endian::k8in32) {
      v = __builtin_bswap32(v);
    } else if constexpr (kEndian == xe::Endian::k16in32) {
      v = (v << 16) | (v >> 16);
    }
    if constexpr (kReset) {
      const bool match = (sizeof(T) == 4 ? v & 0xFFFFFF : v) == reset_index;
      any |= match;
      v = match ? T(~T(0)) : v;
    }
    dst[i] = v;
  }
  return any;
}

template <typename T, bool kReset>
bool ConvertIndicesReset(const uint8_t* src, T* dst, uint32_t count, xe::Endian endian, uint32_t reset_index) {
  switch (endian) {
    case xe::Endian::k8in16: return ConvertIndicesAs<T, xe::Endian::k8in16, kReset>(src, dst, count, reset_index);
    case xe::Endian::k8in32: return ConvertIndicesAs<T, xe::Endian::k8in32, kReset>(src, dst, count, reset_index);
    case xe::Endian::k16in32: return ConvertIndicesAs<T, xe::Endian::k16in32, kReset>(src, dst, count, reset_index);
    default: return ConvertIndicesAs<T, xe::Endian::kNone, kReset>(src, dst, count, reset_index);
  }
}

// Returns whether any index was the reset index (always false when !reset).
template <typename T>
bool ConvertIndices(const uint8_t* src, T* dst, uint32_t count, xe::Endian endian, bool reset, uint32_t reset_index) {
  return reset ? ConvertIndicesReset<T, true>(src, dst, count, endian, reset_index)
               : ConvertIndicesReset<T, false>(src, dst, count, endian, reset_index);
}

}  // namespace

bool Renderer::CreateBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  VmaAllocationCreateInfo alloc{};
  // Host buffers on a discrete GPU stay in system memory: left to VMA they go to the small BAR heap (256 MB without
  // resizable BAR) once they fit, past its budget (B-113). Unified-memory devices keep VMA's choice.
  const VkPhysicalDeviceProperties* physical = nullptr;
  vmaGetPhysicalDeviceProperties(allocator_, &physical);
  const bool discrete = physical && physical->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  alloc.usage = !host ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
                      : discrete ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO;
  if (host) alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  if (tight_memory_) alloc.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  VmaAllocationInfo mapped;
  if (vmaCreateBuffer(allocator_, &info, &alloc, &buffer.buffer, &buffer.allocation, &mapped) != VK_SUCCESS) {
    return false;
  }
  buffer.mapping = static_cast<uint8_t*>(mapped.pMappedData);
  buffer.size = size;
  return !host || buffer.mapping != nullptr;
}

bool Renderer::ImportGuestMemory() {
  const VkDevice dev = device_->device();
  const VkPhysicalDeviceProperties* physical = nullptr;
  vmaGetPhysicalDeviceProperties(allocator_, &physical);
  // On by default only on Apple GPUs (all unified memory), where it's been played; other integrated GPUs (Steam Deck,
  // laptop graphics) can opt in with BOTS_GFX_ZERO_COPY=1 until it's been tested there.
#ifdef __APPLE__
  bool want = physical && physical->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
#else
  bool want = false;
#endif
  if (const char* v = std::getenv("BOTS_GFX_ZERO_COPY"); v && *v) want = *v != '0';
  if (!want || !vk_.vkGetMemoryHostPointerPropertiesEXT) return false;
  // Physical memory has to be one host range (it is in the game; a replay's snapshot may not be).
  auto* host = const_cast<uint8_t*>(memory_(0));
  if (!host || memory_(uint32_t(kSharedMemorySize) - 4096) != host + kSharedMemorySize - 4096) return false;

  VkMemoryHostPointerPropertiesEXT host_props{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
  if (vk_.vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                                              &host_props) != VK_SUCCESS) {
    return false;
  }
  VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
  info.size = kSharedMemorySize;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (vk_.vkCreateBuffer(dev, &info, nullptr, &shared_memory_.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements;
  vk_.vkGetBufferMemoryRequirements(dev, shared_memory_.buffer, &requirements);
  const uint32_t types = requirements.memoryTypeBits & host_props.memoryTypeBits;
  bool ok = types != 0 && reinterpret_cast<uintptr_t>(host) % requirements.alignment == 0;
  if (ok) {
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = host;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
    alloc.allocationSize = kSharedMemorySize;
    alloc.memoryTypeIndex = uint32_t(std::countr_zero(types));
    ok = vk_.vkAllocateMemory(dev, &alloc, nullptr, &imported_memory_) == VK_SUCCESS &&
         vk_.vkBindBufferMemory(dev, shared_memory_.buffer, imported_memory_, 0) == VK_SUCCESS;
  }
  if (!ok) {
    vk_.vkDestroyBuffer(dev, shared_memory_.buffer, nullptr);
    if (imported_memory_) vk_.vkFreeMemory(dev, imported_memory_, nullptr);
    shared_memory_.buffer = VK_NULL_HANDLE;
    imported_memory_ = VK_NULL_HANDLE;
    REXLOG_WARN("gfx native: couldn't import guest memory for the GPU; using a copy of it");
    return false;
  }
  shared_memory_.size = kSharedMemorySize;
  REXLOG_INFO("gfx native: the GPU reads guest memory in place (no copy)");
  return true;
}

bool Renderer::InitDrawing() {
  const VkDevice dev = device_->device();
  const auto& props = device_->properties();
  // Pipeline workers: BOTS_GFX_PIPELINE_THREADS (0: create on the GPU thread), else a third of the hardware
  // threads, 1-4 (the guest's CPU threads come first).
  uint32_t workers = std::clamp(std::thread::hardware_concurrency() / 3, 1u, 4u);
  if (const char* v = std::getenv("BOTS_GFX_PIPELINE_THREADS"); v && *v) workers = uint32_t(std::atoi(v));
  async_pipelines_ = workers != 0;
  // Warm-up-only workers at low priority (BOTS_GFX_WARMUP_THREADS), else the hardware threads left over after the
  // pipeline workers, the guest's main and GPU threads, up to 6.
  const uint32_t hw = std::thread::hardware_concurrency();
  uint32_t warmup = hw > workers + 2 ? std::min(hw - workers - 2, 6u) : 0;
  if (const char* v = std::getenv("BOTS_GFX_WARMUP_THREADS"); v && *v) warmup = uint32_t(std::atoi(v));
  if (!workers) warmup = 0;
  REXLOG_INFO("gfx native: {} pipeline workers, {} warm-up workers", workers, warmup);
  draw_profile_ = std::getenv("BOTS_DRAW_PROFILE") && *std::getenv("BOTS_DRAW_PROFILE") == '1';
  if (const char* v = std::getenv("BOTS_GFX_PASS_MERGE"); v && *v) pass_merge_ = *v == '1';
  if (const char* v = std::getenv("BOTS_GFX_LAZY_RAW_DEPTH"); v && *v) lazy_raw_depth_ = *v == '1';
  if (const char* v = std::getenv("BOTS_GFX_MAX_ANISO"); v && *v) max_anisotropy_ = uint32_t(std::atoi(v));
  scale_ = RenderScale(device_);
  if (scale_ != 1.0f) REXLOG_INFO("gfx native: render scale {}", scale_);
  shaders_ = std::make_unique<ShaderCache>(device_, workers, warmup);
  shared_memory_bindings_ = 1u << Translator::GetSharedMemoryStorageBufferCountLog2(props.maxStorageBufferRange);

  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  if (vk_.vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &set_layout_empty_) != VK_SUCCESS) return false;
  const VkDescriptorSetLayoutBinding shared{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, shared_memory_bindings_, kStages,
                                            nullptr};
  layout_info.bindingCount = 1;
  layout_info.pBindings = &shared;
  if (vk_.vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &set_layout_shared_) != VK_SUCCESS) return false;
  VkDescriptorSetLayoutBinding constants[Translator::kConstantBufferCount];
  for (uint32_t i = 0; i < Translator::kConstantBufferCount; ++i) {
    constants[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, kStages, nullptr};
  }
  layout_info.bindingCount = Translator::kConstantBufferCount;
  layout_info.pBindings = constants;
  if (vk_.vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &set_layout_constants_) != VK_SUCCESS) {
    return false;
  }

  const VkDescriptorPoolSize persistent_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, shared_memory_bindings_},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSubmissions * Translator::kConstantBufferCount}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1 + kSubmissions, 2,
                                       persistent_sizes};
  if (vk_.vkCreateDescriptorPool(dev, &pool_info, nullptr, &persistent_pool_) != VK_SUCCESS) return false;
  const VkDescriptorPoolSize transient_sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTransientSetCount * 8},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kTransientSetCount * 8},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTransientSetCount},
      // The present set's gamma ramp: one per frame. Pools may enforce these counts (Intel's Windows driver
      // does), so every type a transient set uses has to be here.
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
  };
  pool_info.maxSets = kTransientSetCount;
  pool_info.poolSizeCount = uint32_t(std::size(transient_sizes));
  pool_info.pPoolSizes = transient_sizes;
  for (Submission& submission : submissions_) {
    if (vk_.vkCreateDescriptorPool(dev, &pool_info, nullptr, &submission.transient_pool) != VK_SUCCESS) return false;
  }
  const VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, persistent_pool_, 1,
                                          &set_layout_shared_};
  if (vk_.vkAllocateDescriptorSets(dev, &alloc, &shared_set_) != VK_SUCCESS) return false;

  // Guest physical memory, as the translated shaders address it (vertex fetch, index loads). GPU-only: it is filled
  // through the ring (UploadGuestMemory), so on a discrete GPU it lives in VRAM, not pinned system memory (B-113).
  zero_copy_ = ImportGuestMemory();
  bool buffers = zero_copy_ || CreateBuffer(shared_memory_, kSharedMemorySize,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            false);
  for (Buffer& ring : rings_) {
    buffers = buffers && CreateBuffer(ring, kRingSize + kRingSlack, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    ring.size = kRingSize;  // what Upload hands out; the slack is only ever read past
  }
  if (!buffers) {
    std::fprintf(stderr, "native renderer: buffer allocation failed\n");
    return false;
  }
  // One constants set per ring.
  for (uint32_t i = 0; i < kSubmissions; ++i) {
    const VkDescriptorSetAllocateInfo constants_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
                                                      persistent_pool_, 1, &set_layout_constants_};
    if (vk_.vkAllocateDescriptorSets(dev, &constants_alloc, &constant_sets_[i]) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo ranges[Translator::kConstantBufferCount];
    for (uint32_t b = 0; b < Translator::kConstantBufferCount; ++b) ranges[b] = {rings_[i].buffer, 0, kConstantRanges[b]};
    const VkWriteDescriptorSet constants_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, constant_sets_[i], 0, 0,
                                               Translator::kConstantBufferCount,
                                               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, ranges, nullptr};
    vk_.vkUpdateDescriptorSets(dev, 1, &constants_write, 0, nullptr);
  }
  UseSubmission(current_);  // picks up the ring and transient pool just created
  std::vector<VkDescriptorBufferInfo> ranges(shared_memory_bindings_);
  const VkDeviceSize chunk = kSharedMemorySize / shared_memory_bindings_;
  for (uint32_t i = 0; i < shared_memory_bindings_; ++i) ranges[i] = {shared_memory_.buffer, i * chunk, chunk};
  const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shared_set_, 0, 0,
                                   shared_memory_bindings_, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, ranges.data(),
                                   nullptr};
  vk_.vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

  // M5 replaces this with the guest's sampler state.
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.maxLod = VK_LOD_CLAMP_NONE;
  if (vk_.vkCreateSampler(dev, &sampler, nullptr, &sampler_) != VK_SUCCESS) return false;

  // Bound for textures that aren't available yet.
  if (!CreateImage(null_2d_array_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
      !CreateImage(null_3d_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, VK_IMAGE_VIEW_TYPE_3D) ||
      !CreateImage(null_cube_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, VK_IMAGE_VIEW_TYPE_CUBE)) {
    return false;
  }
  // BOTS_NULL_TEXTURE_WHITE=1 shows untextured draws as silhouettes (debugging geometry before textures work).
  const char* white = std::getenv("BOTS_NULL_TEXTURE_WHITE");
  const float fill = white && *white == '1' ? 1.0f : 0.0f;
  const VkClearColorValue black{{fill, fill, fill, fill}};
  for (const Image* image : {&null_2d_array_, &null_3d_, &null_cube_}) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vk_.vkCmdClearColorImage(cmd_, image->image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
  }
  Barrier();
  return InitTransfers();
}

void Renderer::DestroyDrawing() {
  const VkDevice dev = device_->device();
  if (shaders_) shaders_->StopWorkers();
  for (auto& [k, slot] : pipelines_) {
    if (slot->pipeline) vk_.vkDestroyPipeline(dev, slot->pipeline, nullptr);
  }
  for (auto& [k, layout] : pipeline_layouts_) vk_.vkDestroyPipelineLayout(dev, layout.layout, nullptr);
  for (auto& [k, layout] : texture_set_layouts_) vk_.vkDestroyDescriptorSetLayout(dev, layout, nullptr);
  for (VkDescriptorSetLayout layout : {set_layout_shared_, set_layout_constants_, set_layout_empty_}) {
    if (layout) vk_.vkDestroyDescriptorSetLayout(dev, layout, nullptr);
  }
  if (persistent_pool_) vk_.vkDestroyDescriptorPool(dev, persistent_pool_, nullptr);
  for (Submission& submission : submissions_) {
    if (submission.transient_pool) vk_.vkDestroyDescriptorPool(dev, submission.transient_pool, nullptr);
  }
  if (sampler_) vk_.vkDestroySampler(dev, sampler_, nullptr);
  if (zero_copy_) {
    if (shared_memory_.buffer) vk_.vkDestroyBuffer(dev, shared_memory_.buffer, nullptr);
    if (imported_memory_) vk_.vkFreeMemory(dev, imported_memory_, nullptr);
  } else if (shared_memory_.buffer) {
    vmaDestroyBuffer(allocator_, shared_memory_.buffer, shared_memory_.allocation);
  }
  for (Buffer& ring : rings_) {
    if (ring.buffer) vmaDestroyBuffer(allocator_, ring.buffer, ring.allocation);
  }
  shaders_.reset();
}

// Snapshot (no tracker): all of physical memory, once. With a tracker, draws request what they read instead.
bool Renderer::UploadSharedMemory() {
  if (!memory_(0)) return false;
  return UploadGuestMemory(0, uint32_t(kSharedMemorySize));
}

uint8_t* Renderer::Upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
  const VkDeviceSize start = (ring_used_ + alignment - 1) / alignment * alignment;
  upload_max_ = std::max(upload_max_, size);
  if (start + size > ring_.size) {  // IssueDraw reserves enough up front
    ++ring_full_;
    return nullptr;
  }
  offset = start;
  ring_used_ = start + size;
  return ring_.mapping + start;
}

VkDescriptorSet Renderer::AllocateSet(VkDescriptorSetLayout layout) {
  const VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, transient_pool_, 1,
                                          &layout};
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vk_.vkAllocateDescriptorSets(device_->device(), &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
  ++transient_sets_;
  return set;
}

VkDescriptorSetLayout Renderer::TextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers) {
  if (!textures && !samplers) return set_layout_empty_;
  auto& slot = texture_set_layouts_[{vertex, textures, samplers}];
  if (slot) return slot;
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  const VkShaderStageFlags stage = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 0; i < textures; ++i) bindings.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage, nullptr});
  for (uint32_t i = 0; i < samplers; ++i) {
    bindings.push_back({textures + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stage, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  vk_.vkCreateDescriptorSetLayout(device_->device(), &info, nullptr, &slot);
  return slot;
}

const Renderer::PipelineLayout* Renderer::GetPipelineLayout(const LayoutKey& key) {
  auto& slot = pipeline_layouts_[key];
  if (slot.layout) return &slot;
  slot.textures_vertex = TextureSetLayout(true, key[0], key[1]);
  slot.textures_pixel = TextureSetLayout(false, key[2], key[3]);
  const VkDescriptorSetLayout sets[] = {set_layout_shared_, set_layout_constants_, slot.textures_vertex,
                                        slot.textures_pixel};
  VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = 4;
  info.pSetLayouts = sets;
  if (!slot.textures_vertex || !slot.textures_pixel ||
      vk_.vkCreatePipelineLayout(device_->device(), &info, nullptr, &slot.layout) != VK_SUCCESS) {
    return nullptr;
  }
  return &slot;
}

// Thread-safe (worker threads): everything comes from the key and state fixed at construction.
VkPipeline Renderer::CreateDrawPipeline(const PipelineKey& key) const {
  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
       static_cast<const ShaderCache::Translation*>(key.vs)->shader_module(), "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
       key.ps ? static_cast<const ShaderCache::Translation*>(key.ps)->shader_module() : VK_NULL_HANDLE, "main", nullptr},
  };
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VkPrimitiveTopology(key.topology);
  input_assembly.primitiveRestartEnable = key.restart;
  VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = viewport_state.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.depthClampEnable = key.depth_clamp;
  raster.polygonMode = key.polygon_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  raster.cullMode = VkCullModeFlags(key.cull);
  raster.frontFace = key.front_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.depthBiasEnable = VK_TRUE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_stencil.depthTestEnable = key.depth_test;
  depth_stencil.depthWriteEnable = key.depth_write;
  depth_stencil.depthCompareOp = CompareOp(xe::CompareFunction(key.depth_op));
  depth_stencil.stencilTestEnable = key.stencil;
  auto stencil_state = [](const uint32_t ops[4]) {
    VkStencilOpState s{};
    s.failOp = StencilOp(xe::StencilOp(ops[0]));
    s.passOp = StencilOp(xe::StencilOp(ops[1]));
    s.depthFailOp = StencilOp(xe::StencilOp(ops[2]));
    s.compareOp = CompareOp(xe::CompareFunction(ops[3]));
    return s;
  };
  depth_stencil.front = stencil_state(key.front);
  depth_stencil.back = stencil_state(key.back);
  VkPipelineColorBlendAttachmentState blends[4]{};
  for (uint32_t i = 0; i < 4; ++i) {
    blends[i].colorWriteMask = key.write_mask[i];
    reg::RB_BLENDCONTROL bc;
    bc.value = key.blend[i];
    blends[i].srcColorBlendFactor = BlendFactor(bc.color_srcblend);
    blends[i].dstColorBlendFactor = BlendFactor(bc.color_destblend);
    blends[i].colorBlendOp = BlendOp(bc.color_comb_fcn);
    blends[i].srcAlphaBlendFactor = BlendFactor(bc.alpha_srcblend);
    blends[i].dstAlphaBlendFactor = BlendFactor(bc.alpha_destblend);
    blends[i].alphaBlendOp = BlendOp(bc.alpha_comb_fcn);
    // 1 * src + 0 * dst is no blending.
    blends[i].blendEnable = key.write_mask[i] && (key.blend[i] & 0x1FFF1FFF) != 0x00010001;
  }
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 4;
  blend.pAttachments = blends;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR,
                                           VK_DYNAMIC_STATE_DEPTH_BIAS,
                                           VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                           VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                           VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                           VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering_info.colorAttachmentCount = 4;
  rendering_info.pColorAttachmentFormats = key.color;
  rendering_info.depthAttachmentFormat = rendering_info.stencilAttachmentFormat = PassDepthFormat(key.depth);
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering_info};
  info.stageCount = key.ps ? 2 : 1;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport_state;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = key.layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkPipelineCache cache = create_with_cache_ ? pipeline_cache_ : VK_NULL_HANDLE;
  auto hash = [](const void* t) {
    return t ? static_cast<const ShaderCache::Translation*>(t)->shader().ucode_data_hash() : uint64_t(0);
  };
  const auto start = std::chrono::steady_clock::now();
  if (const VkResult result = vk_.vkCreateGraphicsPipelines(device_->device(), cache, 1, &info, nullptr, &pipeline);
      result != VK_SUCCESS) {
    // Its draws are dropped every frame (an object or effect missing), so the log should say (first 50).
    if (pipeline_failures_.fetch_add(1, std::memory_order_relaxed) < 50) {
      REXLOG_ERROR("gfx native: the driver failed to create a pipeline (VkResult {}): vs {:016X} ps {:016X}",
                   int(result), hash(key.vs), hash(key.ps));
    }
    return VK_NULL_HANDLE;
  }
  // A slow driver compile is a skipped (async) or stalled (sync) draw for as long; name the pair.
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (ms >= 250) {
    REXLOG_WARN("gfx native: pipeline took {:.0f} ms: vs {:016X} ps {:016X}", ms, hash(key.vs), hash(key.ps));
  }
  return pipeline;
}

VkPipeline Renderer::GetPipeline(const PipelineKey& key, bool sync, bool& pending, bool background) {
  const std::string_view bytes(reinterpret_cast<const char*>(&key), sizeof(key));
  pending = false;
  if (last_pipeline_ && bytes == last_pipeline_key_) return last_pipeline_;
  auto it = pipelines_.find(bytes);
  if (it == pipelines_.end()) it = pipelines_.emplace(std::string(bytes), std::make_unique<PipelineSlot>()).first;
  PipelineSlot& slot = *it->second;
  // Recorded on first live use, so a pipeline the warm-up made from the master lands in this machine's manifest too
  // once the game draws with it (the manifest is then everything this machine ran, e.g. a coverage playthrough).
  Async state = slot.state.load(std::memory_order_acquire);
  if (!background && state == Async::kNone) NoteLivePipeline(key);  // before RecordPipeline makes it known
  if (!background && !slot.recorded) {
    slot.recorded = true;
    RecordPipeline(key);
  }
  if (state == Async::kNone) {
    Workers* workers = shaders_->workers();
    if (sync || !workers) {
      slot.pipeline = CreateDrawPipeline(key);
      state = slot.pipeline ? Async::kReady : Async::kFailed;
      slot.state.store(state, std::memory_order_release);
    } else {
      slot.state.store(Async::kPending, std::memory_order_relaxed);
      workers->Submit(
          [this, key, s = &slot](rex::graphics::SpirvShaderTranslator&) {
            s->pipeline = CreateDrawPipeline(key);
            s->state.store(s->pipeline ? Async::kReady : Async::kFailed, std::memory_order_release);
          },
          background);
      state = Async::kPending;
    }
  }
  if (state == Async::kPending && sync) {
    shaders_->Wait(slot.state);
    state = slot.state.load(std::memory_order_acquire);
  }
  if (state == Async::kPending) {
    pending = true;
    return VK_NULL_HANDLE;
  }
  if (state == Async::kFailed) return VK_NULL_HANDLE;
  if (!slot.counted) slot.counted = true, ++stats_.pipelines;
  last_pipeline_key_.assign(bytes);  // reuses its capacity
  last_pipeline_ = slot.pipeline;
  return last_pipeline_;
}

// The bounding box (exclusive, guest pixels, plus a pixel of slack) of a screen-space rectangle list whose vertex
// shader reads one float2/3/4 position at the start of a single vertex stream, from that data. False if the draw
// isn't like that.
bool Renderer::RectListBound(const rg::RegisterFile& regs, const ShaderCache::Shader& vshader, xe::SourceSelect source,
                             uint32_t count, uint32_t& x1, uint32_t& y1) const {
  const auto& bindings = vshader.vertex_bindings();
  if (bindings.size() != 1 || bindings[0].attributes.empty() || source != xe::SourceSelect::kAutoIndex || !count ||
      count > 96 || regs.values[rg::XE_GPU_REG_VGT_INDX_OFFSET]) {
    return false;
  }
  const auto& binding = bindings[0];
  const auto& position = binding.attributes[0].fetch_instr.attributes;
  if (position.offset != 0 || (position.data_format != xe::VertexFormat::k_32_32_FLOAT &&
                               position.data_format != xe::VertexFormat::k_32_32_32_FLOAT &&
                               position.data_format != xe::VertexFormat::k_32_32_32_32_FLOAT)) {
    return false;
  }
  const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
  const uint32_t stride = binding.stride_words;
  if (fetch.type != xe::FetchConstantType::kVertex || !stride || (count - 1) * stride + 2 > fetch.size) return false;
  const uint8_t* data = memory_(fetch.address << 2);
  if (!data) return false;
  float max_x = 0.0f, max_y = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    float xy[2];
    std::memcpy(xy, data + 4 * size_t(i * stride), sizeof(xy));
    for (float& v : xy) v = xe::GpuSwap(v, fetch.endian);
    if (!std::isfinite(xy[0]) || !std::isfinite(xy[1])) return false;
    max_x = std::max(max_x, xy[0]), max_y = std::max(max_y, xy[1]);
  }
  if (max_x > 16384.0f || max_y > 16384.0f) return false;
  x1 = uint32_t(std::ceil(max_x + 0.5f)) + 1, y1 = uint32_t(std::ceil(max_y + 0.5f)) + 1;
  return true;
}

// Which draws get skipped on a cold cache (B-119): one that runs once and feeds later frames (exposure, a LUT) would
// stay wrong, not just show up late. Frame = presents so far.
void Renderer::LogSkip(const char* why, uint64_t vs, uint64_t ps, const Attachments* attachments) {
  --log_skips_;
  std::string targets;
  if (attachments) {
    for (const Image* image : attachments->color) {
      if (image) targets += fmt::format(" color {}x{} fmt {}", image->width, image->height, int(image->format));
    }
    if (attachments->depth) targets += fmt::format(" depth {}x{}", attachments->depth->width, attachments->depth->height);
  }
  Workers* workers = shaders_ ? shaders_->workers() : nullptr;
  REXLOG_INFO("gfx native: skipped draw ({}) frame {} draw {}: vs {:016X} ps {:016X}{}; workers {}", why,
              stats_.presents, stats_.draws, vs, ps, targets, workers ? workers->Describe() : "none");
}

void Renderer::IssueDraw(const rg::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps,
                         const Draw& draw) {
  const auto& props = device_->properties();
  const auto mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if ((mode != xe::EdramMode::kColorDepth && mode != xe::EdramMode::kDepthOnly) || !surface_info.surface_pitch) {
    ++stats_.skipped_draws;
    return;
  }
  DrawProfile profile(draw_profile_ ? stats_.draw_phase_ns : nullptr);
  ProcessInvalidations();
  if (!shared_memory_uploaded_ && !tracker_) {
    if (!UploadSharedMemory()) {
      ok_ = false;
      return;
    }
    shared_memory_uploaded_ = true;
  }

  // Shaders.
  auto ucode = [&](const ShaderLoad& s) {
    return s.inline_ucode ? s.inline_ucode : reinterpret_cast<const uint32_t*>(memory_(s.address));
  };
  if (!vs.dwords || !ucode(vs)) {
    ++stats_.skipped_draws;
    return;
  }
  ShaderCache::Shader* vshader = shaders_->Get(xe::ShaderType::kVertex, ucode(vs), vs.dwords);
  const bool polygonal = du::IsPrimitivePolygonal(regs);
  if (!du::IsRasterizationPotentiallyDone(regs, polygonal)) {
    ++stats_.skipped_draws;  // memexport-only draws: not used by this game so far
    return;
  }
  ShaderCache::Shader* pshader = nullptr;
  if (mode == xe::EdramMode::kColorDepth && ps.dwords && ucode(ps)) {
    pshader = shaders_->Get(xe::ShaderType::kPixel, ucode(ps), ps.dwords);
    if (!du::IsPixelShaderNeededWithRasterization(*pshader, regs)) pshader = nullptr;
  }
  // BOTS_SKIP_DRAWS=<a>-<b> leaves draws a..b out (bisecting rendering bugs).
  static const std::pair<long long, long long> skip = [] {
    long long a = -1, b = -1;
    if (const char* v = std::getenv("BOTS_SKIP_DRAWS")) std::sscanf(v, "%lld-%lld", &a, &b);
    return std::pair{a, b};
  }();
  if ((long long)stats_.draws + 1 >= skip.first && (long long)stats_.draws + 1 <= skip.second) {
    ++stats_.draws;
    return;
  }
  // BOTS_DUMP_DRAW=<n> prints the microcode of draw n (numbered as in the draw trace) - debugging.
  static const long long dump_draw = [] {
    const char* v = std::getenv("BOTS_DUMP_DRAW");
    return v ? std::atoll(v) : -1;
  }();
  if (dump_draw == (long long)stats_.draws + 1) {
    std::fprintf(stderr, "--- draw %lld vertex shader %08X\n%s", dump_draw, vs.address,
                 vshader->ucode_disassembly().c_str());
    if (pshader) {
      std::fprintf(stderr, "--- pixel shader %08X\n%s", ps.address, pshader->ucode_disassembly().c_str());
      for (uint32_t c = 0; c < 256; ++c) {
        const uint32_t* v = &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_256_X + 4 * c];
        if (!(v[0] | v[1] | v[2] | v[3])) continue;
        float f[4];
        std::memcpy(f, v, sizeof(f));
        std::fprintf(stderr, "  c%u = %g %g %g %g\n", c, f[0], f[1], f[2], f[3]);
      }
    }
  }
  const reg::RB_DEPTHCONTROL depth_control = du::GetNormalizedDepthControl(regs);
  const auto program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  uint32_t param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      pshader ? vshader->writes_interpolators() &
                    pshader->GetInterpolatorInputMask(program_cntl, regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen_pos)
              : 0;

  // Primitive type -> host topology.
  const xe::PrimitiveType prim = draw.initiator.prim_type;
  const bool rect = prim == xe::PrimitiveType::kRectangleList;
  VkPrimitiveTopology topology;
  switch (prim) {
    case xe::PrimitiveType::kLineList: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case xe::PrimitiveType::kLineStrip: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case xe::PrimitiveType::kTriangleList:
    case xe::PrimitiveType::kTriangleFan:
    case xe::PrimitiveType::kQuadList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case xe::PrimitiveType::kTriangleStrip:
    case xe::PrimitiveType::kRectangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default:
      ++stats_.unsupported_primitives;
      return;
  }
  const auto source = draw.initiator.source_select;
  if (source == xe::SourceSelect::kImmediate) {
    ++stats_.unsupported_primitives;
    return;
  }
  const bool dma = source == xe::SourceSelect::kDMA;
  uint32_t count = draw.initiator.num_indices;
  if (dma) count = std::min(count, draw.index_words);
  if (!count) return;
  const bool index32 = draw.initiator.index_size == xe::IndexFormat::kInt32;

  // Shader modifications (VulkanPipelineCache::GetCurrent*ShaderModification).
  const Translator& translator = shaders_->translator();
  Translator::Modification vmod(translator.GetDefaultVertexShaderModification(
      vshader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
      rect ? rg::Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
           : rg::Shader::HostVertexShaderType::kVertex));
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = clip.clip_disable ? 0 : clip.ucp_ena;
  vmod.vertex.interpolator_mask = interpolator_mask;
  vmod.vertex.user_clip_plane_cull = user_clip_planes && clip.ucp_cull_only_ena && props.shaderCullDistance;
  if (!vmod.vertex.user_clip_plane_cull && !props.shaderClipDistance) user_clip_planes = 0;
  vmod.vertex.user_clip_plane_count = std::popcount(user_clip_planes);
  vmod.vertex.point_ps_ucp_mode = clip.ps_ucp_mode;
  vmod.vertex.output_point_parameters = 0;
  vmod.vertex.vertex_kill_and = props.shaderCullDistance &&
                                (vshader->writes_point_size_edge_flag_kill_vertex() & 0b100) && !clip.vtx_kill_or;
  Translator::Modification pmod(0);
  if (pshader) {
    pmod = Translator::Modification(translator.GetDefaultPixelShaderModification(
        pshader->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
    pmod.pixel.interpolator_mask = interpolator_mask;
    pmod.pixel.interpolators_centroid =
        interpolator_mask & ~xe::GetInterpolatorSamplingPattern(
                                surface_info.msaa_samples, regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (param_gen_pos < xe::kMaxInterpolators) {
      pmod.pixel.param_gen_enable = 1;
      pmod.pixel.param_gen_interpolator = param_gen_pos;
      pmod.pixel.param_gen_point = prim == xe::PrimitiveType::kPointList;
    }
    using DepthStencilMode = Translator::Modification::DepthStencilMode;
    pmod.pixel.depth_stencil_mode =
        pshader->implicit_early_z_write_allowed() &&
                (!pshader->writes_color_target(0) || !du::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
  }
  profile.Mark(kPhaseShaders);
  // Shaders are translated and pipelines created on worker threads; a draw whose aren't ready yet is skipped (a new
  // effect shows up a few frames late instead of stalling the frame). Rectangle lists - XDK clears and screen-space
  // passes, few and cheap - wait instead, as does everything when not live (replay, shadow: exact frames).
  const bool sync = !async_pipelines_ || !tracker_ || rect;
  ShaderCache::Translation *vt = nullptr, *pt = nullptr;
  const Async vs_state = shaders_->Request(*vshader, vmod.value, vt, sync);
  const Async ps_state = pshader ? shaders_->Request(*pshader, pmod.value, pt, sync) : Async::kReady;
  if (vs_state == Async::kFailed || ps_state == Async::kFailed) {
    ++stats_.failed_shaders;
    return;
  }
  if (vs_state != Async::kReady || ps_state != Async::kReady) {
    ++stats_.pending_draws;
    if (log_skips_) {
      LogSkip("shaders", vshader->ucode_data_hash(), pshader ? pshader->ucode_data_hash() : 0, nullptr);
    }
    return;
  }

  profile.Mark(kPhaseTracker);
  // Tracked memory: what the shaders read from shared memory - vertex buffers, and the DMA indices rectangle-list
  // vertex shaders load themselves. (Other indices are read on the CPU below.)
  if (tracker_) {
    const auto& vfetch_bitmap = vshader->constant_register_map().vertex_fetch_bitmap;
    for (uint32_t i = 0; i < std::size(vfetch_bitmap); ++i) {
      for (uint32_t bits = vfetch_bitmap[i]; bits; bits &= bits - 1) {
        const auto fetch = regs.GetVertexFetch(i * 32 + std::countr_zero(bits));
        if (fetch.type == xe::FetchConstantType::kVertex) tracker_->Request(fetch.address << 2, fetch.size << 2);
      }
    }
    if (rect && source == xe::SourceSelect::kDMA) {
      tracker_->Request(draw.index_base, draw.index_words * (index32 ? 4 : 2));
    }
  }

  profile.Mark(kPhaseTextures);
  // Textures. Loading one may record an upload (and flush), so this comes before the ring reservation.
  static const std::vector<rg::SpirvShader::TextureBinding> kNoTextures;
  static const std::vector<rg::SpirvShader::SamplerBinding> kNoSamplers;
  const auto& vtextures = vshader->GetTextureBindingsAfterTranslation();
  const auto& vsamplers = vshader->GetSamplerBindingsAfterTranslation();
  const auto& ptextures = pshader ? pshader->GetTextureBindingsAfterTranslation() : kNoTextures;
  const auto& psamplers = pshader ? pshader->GetSamplerBindingsAfterTranslation() : kNoSamplers;
  static const bool trace = [] {
    const char* v = std::getenv("BOTS_DRAW_TRACE");
    return v && *v == '1';
  }();
  trace_textures_ = trace;
  static const bool trace_signs = std::getenv("BOTS_TRACE_SIGNS") != nullptr;  // fetch swizzle/signs/exp_adjust
  std::string trace_textures;
  uint32_t texture_swizzles[32], texture_signs[32];
  for (uint32_t i = 0; i < 32; ++i) texture_swizzles[i] = xe::XE_GPU_TEXTURE_SWIZZLE_RGBA, texture_signs[i] = 0;
  draw_reads_.clear();
  // At most 32 fetch constants, two bindings each (3D-or-stacked fetches); fixed arrays, no allocation per draw.
  using Views = std::array<VkImageView, 64>;
  auto texture_views = [&](const std::vector<rg::SpirvShader::TextureBinding>& bindings, Views& views) {
    uint32_t n = 0;
    for (const auto& binding : bindings) {
      if (n == views.size()) break;
      const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
      VkImageView view = VK_NULL_HANDLE;
      const char* source = "null";
      // As the SDK's AreDimensionsCompatible: 3D-or-stacked fetches have a 2D array binding (stacked data) and a 3D
      // one, and the shader picks by the fetch's dimension; the binding that does not match gets the null image.
      Image* null_image = binding.dimension == xe::FetchOpDimension::k3DOrStacked ? &null_3d_
                          : binding.dimension == xe::FetchOpDimension::kCube      ? &null_cube_
                                                                                  : &null_2d_array_;
      const bool compatible =
          binding.dimension == xe::FetchOpDimension::k3DOrStacked ? fetch.dimension == xe::DataDimension::k3D
          : binding.dimension == xe::FetchOpDimension::kCube      ? fetch.dimension == xe::DataDimension::kCube
                                                                  : fetch.dimension == xe::DataDimension::k1D ||
                                                                        fetch.dimension == xe::DataDimension::k2DOrStacked;
      const bool flat = fetch.dimension != xe::DataDimension::k3D && fetch.dimension != xe::DataDimension::kCube;
      if (!compatible) {
        view = null_image->view;
      } else if (Image* resolved = flat ? ResolvedTexture(fetch) : nullptr) {
        // Render-to-texture: a resolve destination is sampled straight from its host image.
        uint32_t swizzle = xe::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        view = ResolvedTextureView(*resolved, fetch, binding.is_signed, swizzle);
        draw_reads_.push_back(resolved);
        texture_swizzles[binding.fetch_constant] = swizzle;
        texture_signs[binding.fetch_constant] = rg::texture_util::SwizzleSigns(fetch);
        source = resolved->format == VK_FORMAT_R32_SFLOAT ? "depth" : "rt";
      } else {
        uint32_t swizzle = xe::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        view = TextureView(fetch, binding.is_signed, swizzle);
        if (view) {
          texture_swizzles[binding.fetch_constant] = swizzle;
          texture_signs[binding.fetch_constant] = rg::texture_util::SwizzleSigns(fetch);
          source = "tex";
        } else {
          view = null_image->view;
        }
      }
      views[n++] = view;
      if (trace && !binding.is_signed) {
        char t[96];
        std::snprintf(t, sizeof(t), " t%u=%08X/%u(%s)", uint32_t(binding.fetch_constant), fetch.base_address << 12,
                      uint32_t(fetch.format), source);
        if (trace_signs) {
          std::snprintf(t + std::strlen(t), sizeof(t) - std::strlen(t), "[sw%03o sg%02X x%d]", fetch.swizzle,
                        rg::texture_util::SwizzleSigns(fetch), int(fetch.exp_adjust));
        }
        trace_textures += t;
      }
    }
    return std::span<const VkImageView>(views.data(), n);
  };
  if (vtextures.size() > Views{}.size() || ptextures.size() > Views{}.size()) {
    ++stats_.skipped_draws;
    return;
  }
  Views vview_storage, pview_storage;
  const std::span<const VkImageView> vviews = texture_views(vtextures, vview_storage),
                                     pviews = texture_views(ptextures, pview_storage);

  profile.Mark(kPhaseRing);
  // Room for this draw's uploads and descriptor sets: flush first rather than mid-draw.
  BeginCommands(kGpuDraw);
  const VkDeviceSize upload_need = 64 * 1024 + VkDeviceSize(count) * 12;
  if (upload_need > ring_.size) {
    ++stats_.skipped_draws;
    return;
  }
  if (ring_used_ + upload_need > ring_.size || transient_sets_ + 12 > kTransientSetCount) {
    if (ring_used_ + upload_need > ring_.size) ++ring_full_;
    Flush();
    BeginCommands(kGpuDraw);
  }

  profile.Mark(kPhaseTargets);
  // Render targets.
  const uint32_t color_mask = du::GetNormalizedColorMask(regs, pshader ? pshader->writes_color_targets() : 0);
  Attachments attachments;
  for (uint32_t i = 0; i < 4; ++i) {
    if ((color_mask >> (4 * i)) & 0xF) attachments.color[i] = Surface(regs, false, i);
  }
  if (depth_control.z_enable || depth_control.stencil_enable) attachments.depth = Surface(regs, true, 0);
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (Image* image : attachments.color) {
    if (image) width = std::min(width, image->width), height = std::min(height, image->height);
  }
  if (attachments.depth) {
    width = std::min(width, attachments.depth->width), height = std::min(height, attachments.depth->height);
  }
  if (width == UINT32_MAX) {
    ++stats_.skipped_draws;  // nothing written
    return;
  }

  profile.Mark(kPhaseIndices);
  // Indices: the host index buffer holds guest vertex indices, already byte-swapped.
  bool indexed = false, restart = false;
  uint32_t host_count = count;
  VkDeviceSize index_offset = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT32;
  std::vector<uint32_t> guest, host;
  const xe::Endian index_endian = IndexEndian(draw.index_endian, index32);
  auto load_guest = [&] {
    guest.resize(count);
    if (!dma) {
      for (uint32_t i = 0; i < count; ++i) guest[i] = i;
      return true;
    }
    const uint8_t* src = memory_(draw.index_base);
    if (!src) return false;
    const bool reset_enabled = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;
    const uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v;
      if (index32) {
        std::memcpy(&v, src + 4 * i, 4);
        v = xe::GpuSwap(v, index_endian);
      } else {
        uint16_t h;
        std::memcpy(&h, src + 2 * i, 2);
        v = xe::GpuSwap(h, index_endian);
      }
      if (reset_enabled && (index32 ? v & 0xFFFFFF : v) == reset_index) {
        v = UINT32_MAX;
        restart = true;
      }
      guest[i] = v;
    }
    return true;
  };
  if (rect) {
    // Two-triangle strips; the vertex shader turns host vertex (4 * rect + corner) into guest vertex
    // (3 * rect + corner), loading the guest index itself for DMA draws.
    const uint32_t rects = count / 3;
    if (!rects) return;
    host.reserve(size_t(rects) * 5);
    for (uint32_t r = 0; r < rects; ++r) {
      if (r) host.push_back(UINT32_MAX);
      for (uint32_t k = 0; k < 4; ++k) host.push_back(r * 4 + k);
    }
    restart = true;
  } else if (dma && prim != xe::PrimitiveType::kQuadList && prim != xe::PrimitiveType::kTriangleFan) {
    // Straight from guest memory into the ring. 16-bit indices stay 16-bit unless a reset index in a list (no
    // restart) has to become 0xFFFFFFFF, as the 32-bit path makes it.
    const uint8_t* src = memory_(draw.index_base);
    if (!src) {
      ++stats_.skipped_draws;
      return;
    }
    const bool reset_enabled = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;
    const uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    const bool strip = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    if (index_uploads_epoch_ != ring_epoch_) {
      index_uploads_.clear();
      index_uploads_epoch_ = ring_epoch_;
    }
    const IndexKey key{draw.index_base, count, reset_enabled ? reset_index : 0,
                       uint32_t(index32) | uint32_t(reset_enabled) << 1 | uint32_t(strip) << 2 |
                           uint32_t(index_endian) << 3};
    if (auto it = index_uploads_.find(key); it != index_uploads_.end()) {
      index_offset = it->second.offset;
      index_type = it->second.index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
      restart = it->second.restart;
      indexed = true;
      ++stats_.index_reuses;
    } else if (!index32 && (!reset_enabled || strip)) {
      auto* dst = reinterpret_cast<uint16_t*>(Upload(VkDeviceSize(count) * 2, 4, index_offset));
      restart = ConvertIndices(src, dst, count, index_endian, reset_enabled, reset_index);
      index_type = VK_INDEX_TYPE_UINT16;
    } else if (index32) {
      auto* dst = reinterpret_cast<uint32_t*>(Upload(VkDeviceSize(count) * 4, 4, index_offset));
      restart = ConvertIndices(src, dst, count, index_endian, reset_enabled, reset_index);
    } else {
      // A 16-bit list with a reset index: it becomes 0xFFFFFFFF, as on the general path below.
      std::vector<uint16_t> narrow(count);
      ConvertIndices(src, narrow.data(), count, index_endian, false, 0);
      auto* dst = reinterpret_cast<uint32_t*>(Upload(VkDeviceSize(count) * 4, 4, index_offset));
      for (uint32_t i = 0; i < count; ++i) dst[i] = narrow[i] == reset_index ? UINT32_MAX : narrow[i];
    }
    if (!indexed) {
      restart = restart && strip;  // Vulkan only restarts strips
      indexed = true;
      ++stats_.index_converts;
      index_uploads_[key] = {uint32_t(index_offset), count * (index32 ? 4u : 2u),
                             index_type == VK_INDEX_TYPE_UINT32, restart};
    }
  } else if (prim == xe::PrimitiveType::kQuadList || prim == xe::PrimitiveType::kTriangleFan || dma) {
    if (!load_guest()) {
      ++stats_.skipped_draws;
      return;
    }
    if (prim == xe::PrimitiveType::kQuadList) {
      for (uint32_t q = 0; q + 3 < count; q += 4) {
        for (uint32_t k : {0, 1, 2, 0, 2, 3}) host.push_back(guest[q + k]);
      }
      restart = false;
    } else if (prim == xe::PrimitiveType::kTriangleFan) {
      for (uint32_t i = 1; i + 1 < count; ++i) host.insert(host.end(), {guest[0], guest[i], guest[i + 1]});
      restart = false;
    } else {
      host = std::move(guest);
      // Vulkan only restarts strips.
      restart = restart && (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                            topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
    }
  }
  if (!host.empty() || rect) {
    uint8_t* p = Upload(host.size() * 4, 4, index_offset);
    std::memcpy(p, host.data(), host.size() * 4);
    host_count = uint32_t(host.size());
    indexed = true;
  }
  if (!host_count) return;

  profile.Mark(kPhaseConstants);
  // System constants (VulkanCommandProcessor::UpdateSystemConstantValues, host render targets).
  // Host depth holds the guest value for both depth formats (not float24 halved into [0, 1)): clears, resolves
  // and transfers read and write it as such.
  // Multisampled surfaces are drawn supersampled (Surface()): scale like the SDK's draw resolution scale.
  uint32_t samples_x, samples_y;
  SampleGrid(regs, samples_x, samples_y);
  du::ViewportInfo viewport;
  du::GetHostViewportInfo(regs, samples_x, samples_y, false, props.maxViewportDimensions[0], props.maxViewportDimensions[1], true,
                          depth_control, false, false, pshader && pshader->writes_depth(), viewport);
  Translator::SystemConstants sc;
  std::memset(&sc, 0, sizeof(sc));
  uint32_t flags = 0;
  if (rect && dma) {
    flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (index32) flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    sc.vertex_index_load_address = draw.index_base;
    sc.vertex_index_endian = index_endian;
  } else {
    sc.vertex_index_endian = xe::Endian::kNone;
  }
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (vte.vtx_xy_fmt) flags |= Translator::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt) flags |= Translator::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt) flags |= Translator::kSysFlag_WNotReciprocal;
  if (polygonal) flags |= Translator::kSysFlag_PrimitivePolygonal;
  if (du::IsPrimitiveLine(regs)) flags |= Translator::kSysFlag_PrimitiveLine;
  flags |= uint32_t(surface_info.msaa_samples) << Translator::kSysFlag_MsaaSamples_Shift;
  const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  if (depth_info.depth_format == xe::DepthRenderTargetFormat::kD24FS8) flags |= Translator::kSysFlag_DepthFloat24;
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  const xe::CompareFunction alpha_func =
      color_control.alpha_test_enable ? color_control.alpha_func : xe::CompareFunction::kAlways;
  flags |= uint32_t(alpha_func) << Translator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < 4; ++i) {
    const auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    // Gamma targets are plain UNORM8 here: the shader applies the guest's piecewise-linear gamma.
    if (info.color_format == xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
    }
    int32_t exp_bias = info.color_exp_bias;
    // 16-bit fixed targets are SNORM: map the guest's -32..32 to -1..1.
    if (info.color_format == xe::ColorRenderTargetFormat::k_16_16 ||
        info.color_format == xe::ColorRenderTargetFormat::k_16_16_16_16) {
      exp_bias -= 5;
    }
    const uint32_t scale_bits = 0x3F800000u + uint32_t(exp_bias << 23);
    std::memcpy(&sc.color_exp_bias[i], &scale_bits, 4);
  }
  sc.flags = flags;
  sc.vertex_base_index = regs.Get<int32_t>(rg::XE_GPU_REG_VGT_INDX_OFFSET);
  sc.vertex_index_min = regs.values[rg::XE_GPU_REG_VGT_MIN_VTX_INDX];
  sc.vertex_index_max = regs.values[rg::XE_GPU_REG_VGT_MAX_VTX_INDX];
  {
    float* plane = sc.user_clip_planes[0];
    for (uint32_t planes = user_clip_planes; planes; planes &= planes - 1) {
      std::memcpy(plane, &regs.values[rg::XE_GPU_REG_PA_CL_UCP_0_X + std::countr_zero(planes) * 4], 16);
      plane += 4;
    }
  }
  for (int i = 0; i < 3; ++i) sc.ndc_scale[i] = viewport.ndc_scale[i], sc.ndc_offset[i] = viewport.ndc_offset[i];
  for (uint32_t i = 0; i < 32; ++i) {
    sc.texture_swizzles[i >> 1] |= texture_swizzles[i] << (12 * (i & 1));  // used without view swizzles
    sc.texture_swizzled_signs[i >> 2] |= texture_signs[i] << (8 * (i & 3));
  }
  sc.alpha_test_reference = regs.Get<float>(rg::XE_GPU_REG_RB_ALPHA_REF);
  sc.alpha_to_mask = color_control.alpha_to_mask_enable ? (color_control.value >> 24) | (1u << 8) : 0;
  sc.tessellation_factor_range_min = regs.Get<float>(rg::XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  sc.tessellation_factor_range_max = regs.Get<float>(rg::XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;

  // Constant buffers (VulkanCommandProcessor::UpdateBindings): float constants packed in use order.
  const VkDeviceSize ubo_align = props.minUniformBufferOffsetAlignment;
  uint32_t constant_offsets[Translator::kConstantBufferCount];
  static_assert(Translator::kConstantBufferCount == kConstantBuffers);
  // Reuses the last upload of a buffer when its key matches (see constant_uploads_); key 0: always upload.
  auto reuse = [&](uint32_t index, uint64_t key, const void* shader) {
    ConstantUpload& last = constant_uploads_[index];
    if (!key || last.epoch != ring_epoch_ || last.key != key || last.shader != shader) return false;
    constant_offsets[index] = last.offset;
    ++stats_.constant_reuses;
    return true;
  };
  auto remember = [&](uint32_t index, uint64_t key, const void* shader) {
    constant_uploads_[index] = {ring_epoch_, key, shader, constant_offsets[index]};
  };
  auto put = [&](uint32_t index, const void* data, VkDeviceSize size, uint64_t key) {
    if (reuse(index, key, nullptr)) return;
    VkDeviceSize offset;
    std::memcpy(Upload(size, ubo_align, offset), data, size);
    constant_offsets[index] = uint32_t(offset);
    remember(index, key, nullptr);
  };
  auto put_floats = [&](uint32_t index, const ShaderCache::Shader* shader, uint32_t first_register) {
    // The latest write to a group of 16 constants this shader reads (bitmap word i covers groups 4i..4i+3).
    uint64_t key = 0;
    if (draw.constant_writes) {
      const uint32_t first_group = (first_register - rg::XE_GPU_REG_SHADER_CONSTANT_000_X) >> 6;
      key = 1;
      for (uint32_t i = 0; shader && i < 4; ++i) {
        const uint64_t bits = shader->constant_register_map().float_bitmap[i];
        for (uint32_t g = 0; g < 4; ++g) {
          if (bits >> (16 * g) & 0xFFFF) key = std::max(key, draw.constant_writes[first_group + 4 * i + g] + 1);
        }
      }
    }
    if (reuse(index, key, shader)) return;
    const uint32_t n = shader ? shader->constant_register_map().float_count : 0;
    const VkDeviceSize size = 16 * std::max(n, 1u);
    VkDeviceSize offset;
    uint8_t* p = Upload(size, ubo_align, offset);
    std::memset(p, 0, size);
    for (uint32_t i = 0; shader && i < 4; ++i) {
      for (uint64_t bits = shader->constant_register_map().float_bitmap[i]; bits; bits &= bits - 1) {
        std::memcpy(p, &regs.values[first_register + (i << 8) + (std::countr_zero(bits) << 2)], 16);
        p += 16;
      }
    }
    constant_offsets[index] = uint32_t(offset);
    remember(index, key, shader);
  };
  // System constants are built per draw: compared by value.
  const bool same_system = last_system_constants_.size() == sizeof(sc) &&
                           std::memcmp(last_system_constants_.data(), &sc, sizeof(sc)) == 0;
  if (!same_system) {
    last_system_constants_.assign(reinterpret_cast<const uint8_t*>(&sc), reinterpret_cast<const uint8_t*>(&sc + 1));
    ++system_constants_version_;
  }
  put(Translator::kConstantBufferSystem, &sc, sizeof(sc), system_constants_version_);
  put_floats(Translator::kConstantBufferFloatVertex, vshader, rg::XE_GPU_REG_SHADER_CONSTANT_000_X);
  put_floats(Translator::kConstantBufferFloatPixel, pshader, rg::XE_GPU_REG_SHADER_CONSTANT_256_X);
  const uint64_t* group_writes = draw.constant_writes;
  put(Translator::kConstantBufferBoolLoop, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 4 * (8 + 32),
      group_writes ? group_writes[kBoolLoopConstantGroup] + 1 : 0);
  put(Translator::kConstantBufferFetch, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 4 * 6 * 32,
      group_writes ? group_writes[kFetchConstantGroup] + 1 : 0);

  profile.Mark(kPhaseDescriptors);
  // Descriptor sets.
  const PipelineLayout* layout = GetPipelineLayout({uint32_t(vtextures.size()), uint32_t(vsamplers.size()),
                                                    uint32_t(ptextures.size()), uint32_t(psamplers.size())});
  if (!layout) {
    ++stats_.skipped_draws;
    return;
  }
  VkDescriptorSet sets[4] = {shared_set_, constant_set_, VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkWriteDescriptorSet writes[4];
  uint32_t write_count = 0;
  VkDescriptorImageInfo images[2 * (64 + 32)];
  size_t image_count = 0;
  // Texture sets are reused within a submission when a draw binds exactly what an earlier one did (consecutive draws
  // of a material mostly do). Views and samplers outlive the submission (Retire), so a handle can't be reused in it.
  auto texture_set = [&](VkDescriptorSet& set, VkDescriptorSetLayout set_layout, std::span<const VkImageView> views,
                         const std::vector<rg::SpirvShader::SamplerBinding>& samplers) {
    if ((views.empty() && samplers.empty()) || samplers.size() > 32) return;
    const size_t first = image_count;
    for (VkImageView view : views) images[image_count++] = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    for (const auto& binding : samplers) {
      images[image_count++] = {GetSampler(regs.GetTextureFetch(binding.fetch_constant), binding), VK_NULL_HANDLE,
                               VK_IMAGE_LAYOUT_GENERAL};
    }
    set_key_.assign(reinterpret_cast<const char*>(&set_layout), sizeof(set_layout));
    for (size_t i = first; i < image_count; ++i) {
      const uint64_t handle = images[i].imageView ? uint64_t(images[i].imageView) : uint64_t(images[i].sampler);
      set_key_.append(reinterpret_cast<const char*>(&handle), sizeof(handle));
    }
    if (const auto it = texture_sets_.find(std::string_view(set_key_)); it != texture_sets_.end()) {
      set = it->second;
      image_count = first;
      return;
    }
    set = AllocateSet(set_layout);
    if (!set) return;
    texture_sets_.emplace(set_key_, set);
    if (!views.empty()) {
      writes[write_count++] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, uint32_t(views.size()),
                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images + first, nullptr, nullptr};
    }
    if (!samplers.empty()) {
      writes[write_count++] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, uint32_t(views.size()), 0,
                               uint32_t(samplers.size()), VK_DESCRIPTOR_TYPE_SAMPLER, images + first + views.size(),
                               nullptr, nullptr};
    }
  };
  texture_set(sets[2], layout->textures_vertex, vviews, vsamplers);
  texture_set(sets[3], layout->textures_pixel, pviews, psamplers);
  if ((sets[2] == VK_NULL_HANDLE && !(vtextures.empty() && vsamplers.empty())) ||
      (sets[3] == VK_NULL_HANDLE && !(ptextures.empty() && psamplers.empty()))) {
    ++stats_.skipped_draws;
    return;
  }
  if (write_count) vk_.vkUpdateDescriptorSets(device_->device(), write_count, writes, 0, nullptr);

  profile.Mark(kPhasePipeline);
  // Pipeline (VulkanPipelineCache::GetCurrentStateDescription + EnsurePipelineCreated, simplified).
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs = vt;
  key.ps = pt;
  key.layout = layout->layout;
  key.topology = topology;
  key.restart = restart;
  const auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (polygonal) {
    key.cull = (mode_cntl.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (mode_cntl.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    key.front_clockwise = mode_cntl.face != 0;
    if (props.fillModeNonSolid && mode_cntl.poly_mode == xe::PolygonModeEnable::kDualMode) {
      xe::PolygonType type = xe::PolygonType::kTriangles;
      if (!mode_cntl.cull_front) type = std::min(type, mode_cntl.polymode_front_ptype);
      if (!mode_cntl.cull_back) type = std::min(type, mode_cntl.polymode_back_ptype);
      key.polygon_line = type != xe::PolygonType::kTriangles;
    }
  }
  key.depth_clamp = props.depthClamp && clip.clip_disable;
  if (attachments.depth) {
    key.depth_test = 1;
    key.depth_write = depth_control.z_enable && depth_control.z_write_enable;
    key.depth_op = uint32_t(depth_control.z_enable ? depth_control.zfunc : xe::CompareFunction::kAlways);
    key.depth = attachments.depth->format;
    if (depth_control.stencil_enable) {
      key.stencil = 1;
      key.front[0] = uint32_t(depth_control.stencilfail), key.front[1] = uint32_t(depth_control.stencilzpass);
      key.front[2] = uint32_t(depth_control.stencilzfail), key.front[3] = uint32_t(depth_control.stencilfunc);
      if (polygonal && depth_control.backface_enable) {
        key.back[0] = uint32_t(depth_control.stencilfail_bf), key.back[1] = uint32_t(depth_control.stencilzpass_bf);
        key.back[2] = uint32_t(depth_control.stencilzfail_bf), key.back[3] = uint32_t(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(key.back, key.front, sizeof(key.back));
      }
    }
  }
  GpuMark(kGpuDraw);  // after any transfers Surface() recorded
  du::Scissor scissor;
  du::GetScissor(regs, scissor);
  // Guest samples -> host pixels (render scale), edge by edge.
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t samples = i ? samples_y : samples_x;
    const uint32_t x0 = Host(scissor.offset[i] * samples);
    scissor.extent[i] = Host((scissor.offset[i] + scissor.extent[i]) * samples) - x0;
    scissor.offset[i] = x0;
  }
  const uint32_t viewport_x1 = Host(viewport.xy_offset[0] + viewport.xy_extent[0]);
  const uint32_t viewport_y1 = Host(viewport.xy_offset[1] + viewport.xy_extent[1]);
  const uint32_t sx = std::min(scissor.offset[0], width), sy = std::min(scissor.offset[1], height);
  VkRect2D scissor_rect{{int32_t(sx), int32_t(sy)},
                        {std::min(scissor.extent[0], width - sx), std::min(scissor.extent[1], height - sy)}};
  // How far this draw can write: fragments land inside scissor and viewport (Vulkan always clips x/y to the
  // viewport), with some slack for wide points. Games leave the scissor wide open, so the viewport is the real bound.
  uint32_t scissor_x1 = sx + scissor_rect.extent.width, scissor_y1 = sy + scissor_rect.extent.height;
  if (viewport.xy_extent[0] && viewport.xy_extent[1]) {
    scissor_x1 = std::min(scissor_x1, viewport_x1 + 16);
    scissor_y1 = std::min(scissor_y1, viewport_y1 + 16);
  }
  // Screen-space draws (VTE off) get a viewport bigger than the surface: they don't shape the render area hint.
  bool bounded = viewport.xy_extent[0] && viewport.xy_extent[1] && viewport_x1 <= width && viewport_y1 <= height;
  // Except screen-space rectangle lists - XDK Clear() and the like, with the scissor wide open too - whose vertex
  // shader passes positions (guest pixels) through: their rectangle is in the vertex data. Bounded by it (scissor
  // included, so the draw stays defined if a shader didn't pass them through), they stop counting as covering the
  // whole surface, which surfaces reach to the end of EDRAM.
  if (!bounded && rect) {
    uint32_t x1, y1;
    if (RectListBound(regs, *vshader, source, count, x1, y1)) {
      x1 = std::min(Host(x1 * samples_x), width), y1 = std::min(Host(y1 * samples_y), height);
      scissor_x1 = std::min(scissor_x1, x1), scissor_y1 = std::min(scissor_y1, y1);
      scissor_rect.extent.width = scissor_x1 > sx ? std::min(scissor_rect.extent.width, scissor_x1 - sx) : 0;
      scissor_rect.extent.height = scissor_y1 > sy ? std::min(scissor_rect.extent.height, scissor_y1 - sy) : 0;
      bounded = true;
    }
  }
  // Pass merging. UE3 marks stencil around each light's colour pass with depth-only draws (depth-only, colour,
  // depth-only: three passes per light, ~45 lights a frame). So a draw whose targets are a subset of the open
  // pass's runs in that pass with the extra attachments write-masked, and a depth-only pass also attaches the
  // colour targets its depth surface was last drawn with. Draws only sample resolve destinations, never
  // surfaces, so an extra attachment can't be a feedback loop. The merged attachments must cover what this draw
  // can write; BOTS_GFX_PASS_MERGE=0 turns it off.
  Attachments pass = attachments;
  if (pass_merge_) {
    auto covers = [&](const Image* image) {
      return !image || (image->width >= scissor_x1 && image->height >= scissor_y1);
    };
    bool subset = rendering_ && (!attachments.depth || attachments.depth == open_pass_.depth) &&
                  std::ranges::all_of(open_pass_.color, covers) && covers(open_pass_.depth);
    for (uint32_t i = 0; i < 4 && subset; ++i) {
      subset = !attachments.color[i] || attachments.color[i] == open_pass_.color[i];
    }
    if (subset) {
      pass = open_pass_;
    } else if (attachments.depth && std::ranges::none_of(attachments.color, std::identity{}) &&
               std::ranges::all_of(attachments.depth->companions, covers)) {
      pass.color = attachments.depth->companions;
    }
  }
  uint32_t pass_width = width, pass_height = height;
  for (const Image* image : pass.color) {
    if (image) pass_width = std::min(pass_width, image->width), pass_height = std::min(pass_height, image->height);
  }
  if (pass.depth) {
    pass_width = std::min(pass_width, pass.depth->width), pass_height = std::min(pass_height, pass.depth->height);
  }

  if (pass.depth && !attachments.depth) key.depth = pass.depth->format;  // merged: no test, no writes
  for (uint32_t i = 0; i < 4; ++i) {
    if (pass.color[i]) key.color[i] = pass.color[i]->format;  // merged extras stay write-masked
    if (!attachments.color[i]) continue;
    key.write_mask[i] = (color_mask >> (4 * i)) & 0xF;
    key.blend[i] = regs.values[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
  }

  bool pending = false;
  const VkPipeline pipeline = GetPipeline(key, sync, pending);
  if (!pipeline) {
    ++(pending ? stats_.pending_draws : stats_.skipped_draws);
    if (!pending) ++stats_.failed_pipeline_draws;
    if (pending && log_skips_) {
      LogSkip("pipeline", vshader->ucode_data_hash(), pshader ? pshader->ucode_data_hash() : 0, &attachments);
    }
    return;
  }

  if (scene_fill_seq_ && pass.color[0]) DrawSceneFill(*pass.color[0]);
  profile.Mark(kPhasePass);
  // Render pass: kept open across draws to the same targets, and reopened when a draw samples something written
  // since the last barrier (reopening the same attachments then always barriers: they count as written). An
  // attachment sampled by its own pass is left as the guest had it.
  Image* const pass_writes[5] = {pass.color[0], pass.color[1], pass.color[2], pass.color[3], pass.depth};
  std::erase_if(draw_reads_, [&](Image* image) { return std::ranges::find(pass_writes, image) != std::end(pass_writes); });
  // The render area: this draw's bound and the surfaces' area hints (later draws mostly stay inside it; one that
  // doesn't reopens the pass larger).
  if (rendering_ && open_pass_ == pass && scissor_x1 <= open_area_.width && scissor_y1 <= open_area_.height &&
      !Conflicts(draw_reads_, {})) {
    Track(draw_reads_, {});
  } else {
    EndRendering(!(open_pass_ == pass)                                                     ? "targets"
                 : scissor_x1 > open_area_.width || scissor_y1 > open_area_.height ? "area"
                                                                                    : "sampled");
    // A merged pass opens before the draws that sample what was resolved just ahead of it (a light's shadow map):
    // barrier for anything written outside the pass now rather than reopening the pass for it later.
    const bool outside_writes = pass_merge_ && std::ranges::any_of(written_since_barrier_, [&](const Image* image) {
                                  return std::ranges::find(pass_writes, image) == std::end(pass_writes);
                                });
    if (outside_writes) Barrier();
    Access(draw_reads_, pass_writes);
    VkRenderingAttachmentInfo colors[4];
    for (uint32_t i = 0; i < 4; ++i) {
      colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      colors[i].imageView = pass.color[i] ? pass.color[i]->view : VK_NULL_HANDLE;
      colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth = colors[0];
    depth.imageView = pass.depth ? pass.depth->view : VK_NULL_HANDLE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    uint32_t area_width = std::max(scissor_x1, 1u), area_height = std::max(scissor_y1, 1u);
    for (const Image* image : pass_writes) {
      if (image) area_width = std::max(area_width, image->area_width), area_height = std::max(area_height, image->area_height);
    }
    open_area_ = {std::min(area_width, pass_width), std::min(area_height, pass_height)};
    rendering.renderArea = {{0, 0}, open_area_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 4;
    rendering.pColorAttachments = colors;
    if (pass.depth) rendering.pDepthAttachment = rendering.pStencilAttachment = &depth;
    BeginRendering(rendering, "pass");
    rendering_ = true;
    open_pass_ = pass;
    if (pass.depth && std::ranges::any_of(pass.color, std::identity{})) pass.depth->companions = pass.color;
    if (gpu_timing_) {
      std::string label = "pass " + std::to_string(width) + "x" + std::to_string(height) + " area " +
                          std::to_string(open_area_.width) + "x" + std::to_string(open_area_.height) + " c";
      for (const Image* image : pass.color) label += image ? " " + std::to_string(image->format) : " -";
      label += pass.depth ? " +depth" : "";
      label += samples_x * samples_y > 1 ? " ss" + std::to_string(samples_x * samples_y) : "";
      GpuLabel(kGpuDraw, std::move(label));
    }
    ++stats_.render_passes;
  }

  if (pipeline != bound_pipeline_) {
    vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_pipeline_ = pipeline;
  }
  vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, 0, 2, sets,
                              Translator::kConstantBufferCount, constant_offsets);
  for (uint32_t i = 2; i < 4; ++i) {
    if (sets[i]) {
      vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, i, 1, &sets[i], 0, nullptr);
    }
  }

  profile.Mark(kPhaseRecord);
  // Dynamic state (VulkanCommandProcessor::UpdateDynamicState).
  VkViewport vp{-1.0f, -1.0f, 1.0f, 1.0f, viewport.z_min, viewport.z_max};
  if (viewport.xy_extent[0] && viewport.xy_extent[1]) {
    vp.x = float(viewport.xy_offset[0]) * scale_, vp.y = float(viewport.xy_offset[1]) * scale_;
    vp.width = float(viewport.xy_extent[0]) * scale_, vp.height = float(viewport.xy_extent[1]) * scale_;
  }
  // BOTS_DEBUG_PIXEL_FRACTION=f (0 < f < 1): draws shade only the top-left f x f of their scissor - a wrong picture,
  // for measuring how much of a frame's GPU time is per-pixel (what a lower render resolution could save).
  static const float pixel_fraction = [] {
    const char* v = std::getenv("BOTS_DEBUG_PIXEL_FRACTION");
    const float f = v ? float(std::atof(v)) : 0.0f;
    return f > 0.0f && f < 1.0f ? f : 1.0f;
  }();
  if (pixel_fraction < 1.0f) {
    scissor_rect.extent.width = uint32_t(float(scissor_rect.extent.width) * pixel_fraction);
    scissor_rect.extent.height = uint32_t(float(scissor_rect.extent.height) * pixel_fraction);
  }
  float bias_slope, bias_constant;
  du::GetPreferredFacePolygonOffset(regs, polygonal, bias_slope, bias_constant);
  bias_constant *= depth_info.depth_format == xe::DepthRenderTargetFormat::kD24S8
                       ? du::kD3D10PolygonOffsetFactorUnorm24
                       : du::kD3D10PolygonOffsetFactorFloat24;
  // BOTS_GFX_DEPTH_BIAS_SCALE=f: multiplies the constant depth bias (decals on walls: +2e-5 guest units, about 335
  // float32 ULPs at the primitive's depth). A test knob for decal flicker reports: if a larger value (16) stops it,
  // the decal and its wall don't get the same depth on that GPU; if 0 changes nothing, they do and the bias is unused.
  static const float bias_scale = [] {
    const char* v = std::getenv("BOTS_GFX_DEPTH_BIAS_SCALE");
    return v && *v ? float(std::atof(v)) : 1.0f;
  }();
  bias_constant *= bias_scale;
  const auto ref_front = regs.Get<reg::RB_STENCILREFMASK>();
  const auto ref_back = polygonal && depth_control.backface_enable
                            ? regs.Get<reg::RB_STENCILREFMASK>(rg::XE_GPU_REG_RB_STENCILREFMASK_BF)
                            : ref_front;
  const DynamicState state{vp,
                           scissor_rect,
                           {bias_constant, bias_slope * xe::kPolygonOffsetScaleSubpixelUnit},
                           {regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_GREEN),
                            regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_ALPHA)},
                           {ref_front.stencilref, ref_back.stencilref, ref_front.stencilmask, ref_back.stencilmask,
                            ref_front.stencilwritemask, ref_back.stencilwritemask}};
  const DynamicState* last = dynamic_valid_ ? &dynamic_ : nullptr;
  auto changed = [&](auto member) { return !last || std::memcmp(&(last->*member), &(state.*member), sizeof(state.*member)); };
  if (changed(&DynamicState::viewport)) vk_.vkCmdSetViewport(cmd_, 0, 1, &state.viewport);
  if (changed(&DynamicState::scissor)) vk_.vkCmdSetScissor(cmd_, 0, 1, &state.scissor);
  if (changed(&DynamicState::bias)) vk_.vkCmdSetDepthBias(cmd_, state.bias[0], 0.0f, state.bias[1]);
  if (changed(&DynamicState::blend)) vk_.vkCmdSetBlendConstants(cmd_, state.blend);
  if (changed(&DynamicState::stencil)) {
    const uint32_t* s = state.stencil;
    const uint32_t* o = last ? last->stencil : nullptr;
    if (!o || s[0] != o[0]) vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_BIT, s[0]);
    if (!o || s[1] != o[1]) vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_BACK_BIT, s[1]);
    if (!o || s[2] != o[2]) vk_.vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_FRONT_BIT, s[2]);
    if (!o || s[3] != o[3]) vk_.vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_BACK_BIT, s[3]);
    if (!o || s[4] != o[4]) vk_.vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_FRONT_BIT, s[4]);
    if (!o || s[5] != o[5]) vk_.vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_BACK_BIT, s[5]);
  }
  dynamic_ = state, dynamic_valid_ = true;

  if (gpu_draw_labels_) {
    char label[128];
    std::snprintf(label, sizeof(label), "draw vs %016llX ps %016llX in %ux%u%s%s",
                  (unsigned long long)vshader->ucode_data_hash(),
                  (unsigned long long)(pshader ? pshader->ucode_data_hash() : 0), open_area_.width, open_area_.height,
                  std::ranges::none_of(attachments.color, std::identity{}) ? " depth-only" : "",
                  samples_x * samples_y > 1 ? " ss4" : "");
    GpuLabel(kGpuDraw, label);
  }
  if (drawdump::Active()) {
    std::vector<drawdump::Stream> streams;
    for (const auto& binding : vshader->vertex_bindings()) {
      const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
      if (fetch.type != xe::FetchConstantType::kVertex || streams.size() >= 8) continue;
      drawdump::Stream& st = streams.emplace_back();
      st.fetch_constant = binding.fetch_constant;
      st.address = fetch.address << 2;
      st.stride = binding.stride_words * 4;
      st.endian = uint32_t(fetch.endian);
      st.size = std::min<uint32_t>(fetch.size * 4, drawdump::kMaxStreamBytes);
      st.data = memory_(st.address);
      for (const auto& a : binding.attributes) {
        if (st.attributes.size() >= 16) break;
        st.attributes.push_back({uint32_t(a.fetch_instr.attributes.offset), uint32_t(a.fetch_instr.attributes.data_format)});
      }
    }
    drawdump::Record(&regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_000_X], vshader->ucode_data_hash(),
                     pshader ? pshader->ucode_data_hash() : 0, open_area_.width, open_area_.height,
                     (std::ranges::none_of(attachments.color, std::identity{}) ? 1u : 0u) |
                         (samples_x * samples_y > 1 ? 2u : 0u) | (rect ? 4u : 0u) | (indexed ? 8u : 0u),
                     uint32_t(prim), host_count, streams);
  }
  if (indexed) vk_.vkCmdBindIndexBuffer(cmd_, ring_.buffer, index_offset, index_type);
  BeginDrawQuery();
  if (indexed) {
    vk_.vkCmdDrawIndexed(cmd_, host_count, 1, 0, 0, 0);
  } else {
    vk_.vkCmdDraw(cmd_, host_count, 1, 0, 0);
  }
  EndDrawQuery();
  ++stats_.draws;
  for (Image* image : attachments.color) {
    if (image) image->Use(scissor_x1, scissor_y1, bounded);
  }
  if (attachments.depth) attachments.depth->Use(scissor_x1, scissor_y1, bounded);
  for (uint32_t i = 0; i < 4; ++i) {
    if (attachments.color[i] && key.write_mask[i]) attachments.color[i]->written = ++write_clock_;
  }
  if (attachments.depth && (key.depth_write || key.stencil)) attachments.depth->written = ++write_clock_;
  if (trace) {
    std::fprintf(stderr,
                 "draw %llu: prim %u %s count %u mode %u rt0 %08X %ux%u%s ps %s blend %08X mask %X%s | depth %08X "
                 "dc %08X pitch %u\n",
                 (unsigned long long)stats_.draws, uint32_t(prim), dma ? "dma" : "auto", count, uint32_t(mode),
                 regs.values[rg::XE_GPU_REG_RB_COLOR_INFO], width, height, attachments.depth ? " +depth" : "",
                 pshader ? "yes" : "no", key.blend[0], color_mask, trace_textures.c_str(),
                 regs.values[rg::XE_GPU_REG_RB_DEPTH_INFO], regs.values[rg::XE_GPU_REG_RB_DEPTHCONTROL],
                 uint32_t(surface_info.surface_pitch));
  }
}

}  // namespace bots::gfx::vk

// Scene-feed draw dump (draw_dump.h).
namespace bots::gfx::drawdump {
namespace {
struct DumpState {
  std::string dir = [] {
    const char* v = std::getenv("BOTS_SCENEFEED_DIR");
    return std::string(v ? v : "");
  }();
  FILE* file = nullptr;
  int frames_left = 0;
  uint32_t draw = 0;
  std::chrono::steady_clock::time_point next_check{};
};
DumpState& State() {
  static DumpState st;
  return st;
}
int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void Open(DumpState& st) {
  const int64_t now = NowUs();
  st.file = std::fopen((st.dir + "/draws_" + std::to_string(now) + ".bin").c_str(), "wb");
  if (!st.file) return;
  const uint32_t version = 2;
  std::fwrite("DFR1", 1, 4, st.file);
  std::fwrite(&version, 4, 1, st.file);
  std::fwrite(&now, 8, 1, st.file);
  st.draw = 0;
}
}  // namespace

bool Active() { return State().file != nullptr; }

void Record(const uint32_t* vs_constants, uint64_t vs, uint64_t ps, uint32_t width, uint32_t height, uint32_t flags,
            uint32_t prim, uint32_t count, const std::vector<Stream>& streams) {
  DumpState& st = State();
  if (!st.file) return;
  const uint32_t head[] = {0x31575244u /* "DRW1" */, st.draw++};
  const uint32_t tail[] = {width, height, flags, prim, count, uint32_t(streams.size())};
  std::fwrite(head, 4, 2, st.file);
  std::fwrite(&vs, 8, 1, st.file);
  std::fwrite(&ps, 8, 1, st.file);
  std::fwrite(tail, 4, 6, st.file);
  std::fwrite(vs_constants, 4, 1024, st.file);
  for (const Stream& s : streams) {
    const uint32_t size = s.data ? s.size & ~3u : 0;
    const uint32_t h[] = {s.fetch_constant, s.address, s.stride, s.endian, uint32_t(s.attributes.size()), size};
    std::fwrite(h, 4, 6, st.file);
    for (const auto& a : s.attributes) std::fwrite(&a, 4, 2, st.file);
    if (size) std::fwrite(s.data, 1, size, st.file);
  }
}

void OnPresent() {
  DumpState& st = State();
  if (st.dir.empty()) return;
  if (st.file) {
    std::fclose(st.file);
    st.file = nullptr;
    if (--st.frames_left > 0) Open(st);
    else REXLOG_INFO("scenefeed: draw dump finished");
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < st.next_check) return;
  st.next_check = now + std::chrono::milliseconds(250);
  const std::string req = st.dir + "/draws.req";
  if (FILE* f = std::fopen(req.c_str(), "rb")) {
    int n = 0;
    if (std::fscanf(f, "%d", &n) != 1) n = 0;
    std::fclose(f);
    std::remove(req.c_str());
    if (n > 0) {
      st.frames_left = n;
      Open(st);
      REXLOG_INFO("scenefeed: dumping the next {} frames of draws to {}", n, st.dir);
    }
  }
}
}  // namespace bots::gfx::drawdump
