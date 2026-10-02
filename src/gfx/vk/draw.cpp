// Renderer: guest draws (shader translation, pipelines, constants, indices).
//
// Follows the SDK's VulkanCommandProcessor::IssueDraw (host render target path) for everything the translated
// shaders depend on - modifications, system constants, constant buffer packing, descriptor layout - and does
// its own thing for render targets (renderer.h) and indices (built on the CPU, already byte-swapped).
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

#include "renderer.h"

namespace bots::gfx::vk {
namespace {

namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace du = rex::graphics::draw_util;
using Translator = rg::SpirvShaderTranslator;

constexpr VkDeviceSize kSharedMemorySize = VkDeviceSize(512) << 20;
constexpr VkDeviceSize kRingSize = VkDeviceSize(128) << 20;
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

// Everything that selects a pipeline. Zero-initialised (padding included) and compared as bytes.
struct PipelineKey {
  const void* vs;
  const void* ps;
  VkPipelineLayout layout;
  uint32_t topology, restart, cull, front_clockwise, polygon_line, depth_clamp;
  uint32_t depth_test, depth_write, depth_op, stencil, front[4], back[4];
  VkFormat color[4], depth;
  uint32_t blend[4], write_mask[4];
};

}  // namespace

bool Renderer::CreateBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo mapped;
  if (vmaCreateBuffer(allocator_, &info, &alloc, &buffer.buffer, &buffer.allocation, &mapped) != VK_SUCCESS) {
    return false;
  }
  buffer.mapping = static_cast<uint8_t*>(mapped.pMappedData);
  buffer.size = size;
  return buffer.mapping != nullptr;
}

bool Renderer::InitDrawing() {
  const VkDevice dev = device_->device();
  const auto& props = device_->properties();
  shaders_ = std::make_unique<ShaderCache>(device_);
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
    constants[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kStages, nullptr};
  }
  layout_info.bindingCount = Translator::kConstantBufferCount;
  layout_info.pBindings = constants;
  if (vk_.vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &set_layout_constants_) != VK_SUCCESS) {
    return false;
  }

  const VkDescriptorPoolSize persistent_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, shared_memory_bindings_};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1,
                                       &persistent_size};
  if (vk_.vkCreateDescriptorPool(dev, &pool_info, nullptr, &persistent_pool_) != VK_SUCCESS) return false;
  const VkDescriptorPoolSize transient_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kTransientSetCount * Translator::kConstantBufferCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTransientSetCount * 8},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kTransientSetCount * 8},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTransientSetCount},
  };
  pool_info.maxSets = kTransientSetCount;
  pool_info.poolSizeCount = 4;
  pool_info.pPoolSizes = transient_sizes;
  for (Submission& submission : submissions_) {
    if (vk_.vkCreateDescriptorPool(dev, &pool_info, nullptr, &submission.transient_pool) != VK_SUCCESS) return false;
  }
  const VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, persistent_pool_, 1,
                                          &set_layout_shared_};
  if (vk_.vkAllocateDescriptorSets(dev, &alloc, &shared_set_) != VK_SUCCESS) return false;

  // Guest physical memory, as the translated shaders address it (vertex fetch, index loads).
  bool buffers = CreateBuffer(shared_memory_, kSharedMemorySize,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  for (Buffer& ring : rings_) {
    buffers = buffers && CreateBuffer(ring, kRingSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  }
  if (!buffers) {
    std::fprintf(stderr, "native renderer: buffer allocation failed\n");
    return false;
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
  for (auto& [k, pipeline] : pipelines_) vk_.vkDestroyPipeline(dev, pipeline, nullptr);
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
  if (shared_memory_.buffer) vmaDestroyBuffer(allocator_, shared_memory_.buffer, shared_memory_.allocation);
  for (Buffer& ring : rings_) {
    if (ring.buffer) vmaDestroyBuffer(allocator_, ring.buffer, ring.allocation);
  }
  shaders_.reset();
}

// Snapshot (no tracker): all of physical memory, once. With a tracker, draws request what they read instead.
bool Renderer::UploadSharedMemory() {
  const uint8_t* source = memory_(0);
  if (!source) return false;
  std::memcpy(shared_memory_.mapping, source, kSharedMemorySize);
  vmaFlushAllocation(allocator_, shared_memory_.allocation, 0, VK_WHOLE_SIZE);
  return true;
}

uint8_t* Renderer::Upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
  const VkDeviceSize start = (ring_used_ + alignment - 1) / alignment * alignment;
  if (start + size > ring_.size) return nullptr;  // IssueDraw reserves enough up front
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

VkPipeline Renderer::GetPipeline(std::string_view key, const VkGraphicsPipelineCreateInfo& info) {
  if (last_pipeline_ && key == last_pipeline_key_) return last_pipeline_;
  last_pipeline_key_.assign(key);  // reuses its capacity
  auto it = pipelines_.find(last_pipeline_key_);
  if (it == pipelines_.end()) {
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vk_.vkCreateGraphicsPipelines(device_->device(), pipeline_cache_, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
      std::fprintf(stderr, "native renderer: pipeline creation failed\n");
      pipeline = VK_NULL_HANDLE;
    } else {
      ++stats_.pipelines;
    }
    it = pipelines_.emplace(last_pipeline_key_, pipeline).first;
  }
  last_pipeline_ = it->second;
  return last_pipeline_;
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
  ShaderCache::Translation* vt = shaders_->Translate(*vshader, vmod.value);
  ShaderCache::Translation* pt = pshader ? shaders_->Translate(*pshader, pmod.value) : nullptr;
  if (!vt || (pshader && !pt)) {
    ++stats_.failed_shaders;
    return;
  }

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
  auto texture_views = [&](const std::vector<rg::SpirvShader::TextureBinding>& bindings) {
    std::vector<VkImageView> views;
    for (const auto& binding : bindings) {
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
      views.push_back(view);
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
    return views;
  };
  const std::vector<VkImageView> vviews = texture_views(vtextures), pviews = texture_views(ptextures);

  // Room for this draw's uploads and descriptor sets: flush first rather than mid-draw.
  BeginCommands(kGpuDraw);
  const VkDeviceSize upload_need = 64 * 1024 + VkDeviceSize(count) * 12;
  if (upload_need > ring_.size) {
    ++stats_.skipped_draws;
    return;
  }
  if (ring_used_ + upload_need > ring_.size || transient_sets_ + 12 > kTransientSetCount) {
    Flush();
    BeginCommands(kGpuDraw);
  }

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
    if (!index32 && (!reset_enabled || strip)) {
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
    restart = restart && strip;  // Vulkan only restarts strips
    indexed = true;
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
  VkDescriptorBufferInfo buffers[Translator::kConstantBufferCount];
  auto put = [&](uint32_t index, const void* data, VkDeviceSize size) {
    VkDeviceSize offset;
    std::memcpy(Upload(size, ubo_align, offset), data, size);
    buffers[index] = {ring_.buffer, offset, size};
  };
  auto put_floats = [&](uint32_t index, const ShaderCache::Shader* shader, uint32_t first_register) {
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
    buffers[index] = {ring_.buffer, offset, size};
  };
  put(Translator::kConstantBufferSystem, &sc, sizeof(sc));
  put_floats(Translator::kConstantBufferFloatVertex, vshader, rg::XE_GPU_REG_SHADER_CONSTANT_000_X);
  put_floats(Translator::kConstantBufferFloatPixel, pshader, rg::XE_GPU_REG_SHADER_CONSTANT_256_X);
  put(Translator::kConstantBufferBoolLoop, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 4 * (8 + 32));
  put(Translator::kConstantBufferFetch, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 4 * 6 * 32);

  // Descriptor sets.
  const PipelineLayout* layout = GetPipelineLayout({uint32_t(vtextures.size()), uint32_t(vsamplers.size()),
                                                    uint32_t(ptextures.size()), uint32_t(psamplers.size())});
  if (!layout) {
    ++stats_.skipped_draws;
    return;
  }
  VkDescriptorSet sets[4] = {shared_set_, AllocateSet(set_layout_constants_), VK_NULL_HANDLE, VK_NULL_HANDLE};
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorImageInfo> images;
  images.reserve(vtextures.size() + vsamplers.size() + ptextures.size() + psamplers.size());
  writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[1], 0, 0, Translator::kConstantBufferCount,
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, buffers, nullptr});
  auto texture_set = [&](VkDescriptorSet& set, VkDescriptorSetLayout set_layout, const std::vector<VkImageView>& views,
                         const std::vector<rg::SpirvShader::SamplerBinding>& samplers) {
    if (views.empty() && samplers.empty()) return;
    set = AllocateSet(set_layout);
    const size_t first = images.size();
    for (VkImageView view : views) images.push_back({VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL});
    for (const auto& binding : samplers) {
      images.push_back(
          {GetSampler(regs.GetTextureFetch(binding.fetch_constant), binding), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL});
    }
    if (!views.empty()) {
      writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, uint32_t(views.size()),
                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images.data() + first, nullptr, nullptr});
    }
    if (!samplers.empty()) {
      writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, uint32_t(views.size()), 0,
                        uint32_t(samplers.size()), VK_DESCRIPTOR_TYPE_SAMPLER, images.data() + first + views.size(),
                        nullptr, nullptr});
    }
  };
  texture_set(sets[2], layout->textures_vertex, vviews, vsamplers);
  texture_set(sets[3], layout->textures_pixel, pviews, psamplers);
  if (!sets[1] || (sets[2] == VK_NULL_HANDLE && !(vtextures.empty() && vsamplers.empty())) ||
      (sets[3] == VK_NULL_HANDLE && !(ptextures.empty() && psamplers.empty()))) {
    ++stats_.skipped_draws;
    return;
  }
  vk_.vkUpdateDescriptorSets(device_->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);

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
  for (uint32_t i = 0; i < 4; ++i) {
    if (!attachments.color[i]) continue;
    key.color[i] = attachments.color[i]->format;
    key.write_mask[i] = (color_mask >> (4 * i)) & 0xF;
    key.blend[i] = regs.values[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
       vt->shader_module(), "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
       pt ? pt->shader_module() : VK_NULL_HANDLE, "main", nullptr},
  };
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = topology;
  input_assembly.primitiveRestartEnable = restart;
  VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = viewport_state.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.depthClampEnable = key.depth_clamp;
  raster.polygonMode = key.polygon_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  raster.cullMode = key.cull;
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
  rendering_info.depthAttachmentFormat = rendering_info.stencilAttachmentFormat = key.depth;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering_info};
  info.stageCount = pt ? 2 : 1;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport_state;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout->layout;
  const VkPipeline pipeline = GetPipeline(std::string_view(reinterpret_cast<const char*>(&key), sizeof(key)), info);
  if (!pipeline) {
    ++stats_.skipped_draws;
    return;
  }

  GpuMark(kGpuDraw);  // after any transfers Surface() recorded
  du::Scissor scissor;
  du::GetScissor(regs, scissor);
  scissor.offset[0] *= samples_x, scissor.extent[0] *= samples_x;
  scissor.offset[1] *= samples_y, scissor.extent[1] *= samples_y;
  const uint32_t sx = std::min(scissor.offset[0], width), sy = std::min(scissor.offset[1], height);
  const VkRect2D scissor_rect{{int32_t(sx), int32_t(sy)},
                              {std::min(scissor.extent[0], width - sx), std::min(scissor.extent[1], height - sy)}};
  // How far this draw can write: fragments land inside scissor and viewport (Vulkan always clips x/y to the
  // viewport), with some slack for wide points. Games leave the scissor wide open, so the viewport is the real bound.
  uint32_t scissor_x1 = sx + scissor_rect.extent.width, scissor_y1 = sy + scissor_rect.extent.height;
  if (viewport.xy_extent[0] && viewport.xy_extent[1]) {
    scissor_x1 = std::min(scissor_x1, viewport.xy_offset[0] + viewport.xy_extent[0] + 16);
    scissor_y1 = std::min(scissor_y1, viewport.xy_offset[1] + viewport.xy_extent[1] + 16);
  }
  // Screen-space draws (VTE off) get a viewport bigger than the surface: they don't shape the render area hint.
  const bool bounded = viewport.xy_extent[0] && viewport.xy_extent[1] &&
                       viewport.xy_offset[0] + viewport.xy_extent[0] <= width &&
                       viewport.xy_offset[1] + viewport.xy_extent[1] <= height;
  // Render pass: kept open across draws to the same targets, and reopened when a draw samples something written
  // since the last barrier (reopening the same attachments then always barriers: they count as written). An
  // attachment sampled by its own pass is left as the guest had it.
  Image* const pass_writes[5] = {attachments.color[0], attachments.color[1], attachments.color[2],
                                 attachments.color[3], attachments.depth};
  std::erase_if(draw_reads_, [&](Image* image) { return std::ranges::find(pass_writes, image) != std::end(pass_writes); });
  // The render area: this draw's bound and the surfaces' area hints (later draws mostly stay inside it; one that
  // doesn't reopens the pass larger).
  if (rendering_ && open_pass_ == attachments && scissor_x1 <= open_area_.width && scissor_y1 <= open_area_.height &&
      !Conflicts(draw_reads_, {})) {
    Track(draw_reads_, {});
  } else {
    EndRendering();
    Access(draw_reads_, pass_writes);
    VkRenderingAttachmentInfo colors[4];
    for (uint32_t i = 0; i < 4; ++i) {
      colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      colors[i].imageView = attachments.color[i] ? attachments.color[i]->view : VK_NULL_HANDLE;
      colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth = colors[0];
    depth.imageView = attachments.depth ? attachments.depth->view : VK_NULL_HANDLE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    uint32_t area_width = std::max(scissor_x1, 1u), area_height = std::max(scissor_y1, 1u);
    for (const Image* image : pass_writes) {
      if (image) area_width = std::max(area_width, image->area_width), area_height = std::max(area_height, image->area_height);
    }
    open_area_ = {std::min(area_width, width), std::min(area_height, height)};
    rendering.renderArea = {{0, 0}, open_area_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 4;
    rendering.pColorAttachments = colors;
    if (attachments.depth) rendering.pDepthAttachment = rendering.pStencilAttachment = &depth;
    vk_.vkCmdBeginRendering(cmd_, &rendering);
    rendering_ = true;
    open_pass_ = attachments;
    if (gpu_timing_) {
      std::string label = "pass " + std::to_string(width) + "x" + std::to_string(height) + " area " +
                          std::to_string(open_area_.width) + "x" + std::to_string(open_area_.height) + " c";
      for (const Image* image : attachments.color) label += image ? " " + std::to_string(image->format) : " -";
      label += attachments.depth ? " +depth" : "";
      label += samples_x * samples_y > 1 ? " ss" + std::to_string(samples_x * samples_y) : "";
      GpuLabel(kGpuDraw, std::move(label));
    }
    ++stats_.render_passes;
  }

  if (pipeline != bound_pipeline_) {
    vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_pipeline_ = pipeline;
  }
  vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, 0, 2, sets, 0, nullptr);
  for (uint32_t i = 2; i < 4; ++i) {
    if (sets[i]) {
      vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, i, 1, &sets[i], 0, nullptr);
    }
  }

  // Dynamic state (VulkanCommandProcessor::UpdateDynamicState).
  VkViewport vp{-1.0f, -1.0f, 1.0f, 1.0f, viewport.z_min, viewport.z_max};
  if (viewport.xy_extent[0] && viewport.xy_extent[1]) {
    vp.x = float(viewport.xy_offset[0]), vp.y = float(viewport.xy_offset[1]);
    vp.width = float(viewport.xy_extent[0]), vp.height = float(viewport.xy_extent[1]);
  }
  vk_.vkCmdSetViewport(cmd_, 0, 1, &vp);
  vk_.vkCmdSetScissor(cmd_, 0, 1, &scissor_rect);
  float bias_slope, bias_constant;
  du::GetPreferredFacePolygonOffset(regs, polygonal, bias_slope, bias_constant);
  bias_constant *= depth_info.depth_format == xe::DepthRenderTargetFormat::kD24S8
                       ? du::kD3D10PolygonOffsetFactorUnorm24
                       : du::kD3D10PolygonOffsetFactorFloat24;
  vk_.vkCmdSetDepthBias(cmd_, bias_constant, 0.0f, bias_slope * xe::kPolygonOffsetScaleSubpixelUnit);
  const float blend_constants[4] = {
      regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(rg::XE_GPU_REG_RB_BLEND_ALPHA)};
  vk_.vkCmdSetBlendConstants(cmd_, blend_constants);
  const auto ref_front = regs.Get<reg::RB_STENCILREFMASK>();
  const auto ref_back = polygonal && depth_control.backface_enable
                            ? regs.Get<reg::RB_STENCILREFMASK>(rg::XE_GPU_REG_RB_STENCILREFMASK_BF)
                            : ref_front;
  vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_BIT, ref_front.stencilref);
  vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_BACK_BIT, ref_back.stencilref);
  vk_.vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_FRONT_BIT, ref_front.stencilmask);
  vk_.vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_BACK_BIT, ref_back.stencilmask);
  vk_.vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_FRONT_BIT, ref_front.stencilwritemask);
  vk_.vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_BACK_BIT, ref_back.stencilwritemask);

  if (indexed) {
    vk_.vkCmdBindIndexBuffer(cmd_, ring_.buffer, index_offset, index_type);
    vk_.vkCmdDrawIndexed(cmd_, host_count, 1, 0, 0, 0);
  } else {
    vk_.vkCmdDraw(cmd_, host_count, 1, 0, 0);
  }
  ++stats_.draws;
  for (Image* image : pass_writes) {
    if (image) image->Use(scissor_x1, scissor_y1, bounded);
  }
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
