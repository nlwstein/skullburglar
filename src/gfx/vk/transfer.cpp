// Renderer: EDRAM ownership transfers between surfaces of different formats.
//
// On Xenos a surface's format is only how the render backend interprets EDRAM; games render with one format and
// read the same tiles back with another (UE3 writes HDR scene colour as 7e3 and copies its raw bits out through a
// 2_10_10_10 view, and later restores it the other way). Each format here is its own host image, so when a
// surface is used while another one at the same base and pitch holds newer data, a full-screen pass re-encodes
// that data through the guest's 32-bit texel bits. Only 32bpp colour formats so far.
#include <algorithm>
#include <cstdio>
#include <string>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>

#include "renderer.h"

namespace bots::gfx::vk {
namespace {

namespace xe = rex::graphics::xenos;

constexpr const char* kVertexShader = R"(#version 450
void main() {
  // One triangle covering the target.
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// SRC and DST are ColorRenderTargetFormat values (storage formats); SRC 100 is a D24S8 depth/stencil surface.
// The source is scaled to the destination: a 4x MSAA surface covers the tiles of a 1x one twice its size.
constexpr const char* kFragmentShader = R"(
layout(set = 0, binding = 0) uniform sampler2D source;
#if SRC == 100
layout(set = 0, binding = 1) uniform usampler2D stencil;
#endif
layout(push_constant) uniform Constants { ivec2 dest_size; };
layout(location = 0) out vec4 result;

uint To7e3(float f) {
  uint u = floatBitsToUint(clamp(f, 0.0, 31.875));
  uint b = u < 0x3E800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23), 24u) : u - (124u << 23);
  return ((b + 0x7FFFu + ((b >> 16) & 1u)) >> 16) & 0x3FFu;
}
float From7e3(uint v) {
  uint m = v & 0x7Fu, e = (v >> 7) & 7u;
  return e == 0u ? float(m) * exp2(-9.0) : (1.0 + float(m) / 128.0) * exp2(float(e) - 3.0);
}
uint ToUnorm(float f, float scale) { return uint(round(clamp(f, 0.0, 1.0) * scale)); }

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) * textureSize(source, 0) / dest_size;
  vec4 v = texelFetch(source, p, 0);
  uint bits;
#if SRC == 100
  bits = (uint(round(clamp(v.r, 0.0, 1.0) * 16777215.0)) << 8) | (texelFetch(stencil, p, 0).r & 0xFFu);
#elif SRC == 0
  bits = packUnorm4x8(v);
#elif SRC == 2
  bits = ToUnorm(v.r, 1023.0) | (ToUnorm(v.g, 1023.0) << 10) | (ToUnorm(v.b, 1023.0) << 20) | (ToUnorm(v.a, 3.0) << 30);
#elif SRC == 3
  bits = To7e3(v.r) | (To7e3(v.g) << 10) | (To7e3(v.b) << 20) | (ToUnorm(v.a, 3.0) << 30);
#elif SRC == 4
  bits = packSnorm2x16(v.rg);
#elif SRC == 6
  bits = packHalf2x16(v.rg);
#else
  bits = floatBitsToUint(v.r);
#endif
#if DST == 0
  result = unpackUnorm4x8(bits);
#elif DST == 2
  result = vec4(uvec4(bits, bits >> 10, bits >> 20, 0u) & uvec4(0x3FFu)) / 1023.0;
  result.a = float(bits >> 30) / 3.0;
#elif DST == 3
  result = vec4(From7e3(bits), From7e3(bits >> 10), From7e3(bits >> 20), float(bits >> 30) / 3.0);
#elif DST == 4
  result = vec4(unpackSnorm2x16(bits), 0.0, 0.0);
#elif DST == 6
  result = vec4(unpackHalf2x16(bits), 0.0, 0.0);
#else
  result = vec4(uintBitsToFloat(bits), 0.0, 0.0, 0.0);
#endif
}
)";

bool CompileGlsl(EShLanguage stage, const std::string& source, std::vector<uint32_t>& spirv) {
  static const bool initialized = glslang::InitializeProcess();
  (void)initialized;
  TBuiltInResource resources{};
  resources.maxDrawBuffers = 8;
  resources.maxTextureImageUnits = 32;
  resources.maxCombinedTextureImageUnits = 32;
  resources.maxVertexAttribs = 16;
  resources.maxClipDistances = 8;
  resources.limits.generalUniformIndexing = true;
  resources.limits.generalVariableIndexing = true;
  resources.limits.nonInductiveForLoops = true;
  resources.limits.whileLoops = true;
  resources.limits.doWhileLoops = true;
  glslang::TShader shader(stage);
  const char* text = source.c_str();
  shader.setStrings(&text, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  glslang::TProgram program;
  if (!shader.parse(&resources, 450, false, messages)) {
    std::fprintf(stderr, "native renderer: transfer shader: %s\n", shader.getInfoLog());
    return false;
  }
  program.addShader(&shader);
  if (!program.link(messages) || !program.getIntermediate(stage)) {
    std::fprintf(stderr, "native renderer: transfer shader link: %s\n", program.getInfoLog());
    return false;
  }
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
  return !spirv.empty();
}

}  // namespace

bool Renderer::IsTransferable(uint32_t format) {
  switch (xe::ColorRenderTargetFormat(format)) {
    case xe::ColorRenderTargetFormat::k_8_8_8_8:
    case xe::ColorRenderTargetFormat::k_2_10_10_10:
    case xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xe::ColorRenderTargetFormat::k_16_16:
    case xe::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xe::ColorRenderTargetFormat::k_32_FLOAT:
      return true;
    default:
      return false;
  }
}

VkShaderModule Renderer::CreateShaderModule(int stage, const std::string& source) {
  std::vector<uint32_t> spirv;
  if (!CompileGlsl(EShLanguage(stage), source, spirv)) return VK_NULL_HANDLE;
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = spirv.size() * 4;
  info.pCode = spirv.data();
  VkShaderModule module = VK_NULL_HANDLE;
  vk_.vkCreateShaderModule(device_->device(), &info, nullptr, &module);
  if (module) transfer_modules_.push_back(module);
  return module;
}

bool Renderer::InitTransfers() {
  const VkDescriptorSetLayoutBinding bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 2;
  set_info.pBindings = bindings;
  if (vk_.vkCreateDescriptorSetLayout(device_->device(), &set_info, nullptr, &transfer_set_layout_) != VK_SUCCESS) {
    return false;
  }
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &transfer_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (vk_.vkCreatePipelineLayout(device_->device(), &layout_info, nullptr, &transfer_layout_) != VK_SUCCESS) {
    return false;
  }
  transfer_vs_ = CreateShaderModule(EShLangVertex, kVertexShader);
  return transfer_vs_ != VK_NULL_HANDLE;
}

void Renderer::DestroyTransfers() {
  const VkDevice dev = device_->device();
  for (auto& [k, pipeline] : transfer_pipelines_) vk_.vkDestroyPipeline(dev, pipeline, nullptr);
  for (VkShaderModule module : transfer_modules_) vk_.vkDestroyShaderModule(dev, module, nullptr);
  if (transfer_layout_) vk_.vkDestroyPipelineLayout(dev, transfer_layout_, nullptr);
  if (transfer_set_layout_) vk_.vkDestroyDescriptorSetLayout(dev, transfer_set_layout_, nullptr);
}

VkPipeline Renderer::TransferPipeline(uint32_t source_format, uint32_t dest_format, VkFormat host_format) {
  VkPipeline& pipeline = transfer_pipelines_[{source_format, dest_format}];
  if (pipeline) return pipeline;
  const std::string source = "#version 450\n#define SRC " + std::to_string(source_format) + "\n#define DST " +
                             std::to_string(dest_format) + "\n" + kFragmentShader;
  const VkShaderModule fs = CreateShaderModule(EShLangFragment, source);
  if (!fs) return VK_NULL_HANDLE;
  const VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, transfer_vs_,
       "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main",
       nullptr},
  };
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &host_format;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = transfer_layout_;
  if (vk_.vkCreateGraphicsPipelines(device_->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
    pipeline = VK_NULL_HANDLE;
  }
  return pipeline;
}

void Renderer::Transfer(Image& source, uint32_t source_format, Image& dest, uint32_t dest_format) {
  const VkPipeline pipeline = TransferPipeline(source_format, dest_format, dest.format);
  if (!pipeline) return;
  BeginCommands();
  if (transient_sets_ + 1 > kTransientSetCount) {
    Flush();
    BeginCommands();
  }
  const VkDescriptorSet set = AllocateSet(transfer_set_layout_);
  if (!set) return;
  const bool depth = source_format == kTransferDepth;
  const VkDescriptorImageInfo images[2] = {{sampler_, depth ? source.depth_view : source.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {sampler_, source.stencil_view, VK_IMAGE_LAYOUT_GENERAL}};
  const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, depth ? 2u : 1u,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images, nullptr, nullptr};
  vk_.vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);

  const uint32_t width = dest.width, height = dest.height;
  EndRendering();
  Barrier();
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = dest.view;
  color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {width, height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vk_.vkCmdBeginRendering(cmd_, &rendering);
  vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_layout_, 0, 1, &set, 0, nullptr);
  const VkViewport vp{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  vk_.vkCmdSetViewport(cmd_, 0, 1, &vp);
  vk_.vkCmdSetScissor(cmd_, 0, 1, &rendering.renderArea);
  const int32_t dest_size[2] = {int32_t(width), int32_t(height)};
  vk_.vkCmdPushConstants(cmd_, transfer_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dest_size), dest_size);
  vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
  vk_.vkCmdEndRendering(cmd_);
  Barrier();
  ++stats_.transfers;
}

}  // namespace bots::gfx::vk
