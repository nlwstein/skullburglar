// Renderer: native present. The front buffer goes through the guest's 256-entry gamma ramp into the presenter's
// guest output image, as the emulated swap does for 8_8_8_8 front buffers.
#include <cstring>

#include <glslang/Public/ShaderLang.h>

#include "renderer.h"

namespace bots::gfx::vk {
namespace {

// std140 packs the 256 ramp entries as 64 uvec4s.
constexpr const char* kPresentShader = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2DArray source;
layout(set = 0, binding = 1) uniform Gamma { uvec4 ramp[64]; };
layout(location = 0) out vec4 result;

uint Ramp(float v) {
  uint i = uint(round(clamp(v, 0.0, 1.0) * 255.0));
  return ramp[i >> 2][i & 3u];
}

void main() {
  vec3 c = texelFetch(source, ivec3(ivec2(gl_FragCoord.xy), 0), 0).rgb;
  result = vec4(float((Ramp(c.r) >> 20) & 0x3FFu), float((Ramp(c.g) >> 10) & 0x3FFu), float(Ramp(c.b) & 0x3FFu),
                1023.0) / 1023.0;
}
)";

constexpr VkFormat kGuestOutputFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
constexpr VkDeviceSize kRampBytes = 256 * 4;

}  // namespace

bool Renderer::InitPresent() {
  if (present_pipeline_) return true;
  const VkDevice dev = device_->device();
  if (!present_set_layout_) {
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    if (vk_.vkCreateDescriptorSetLayout(dev, &set_info, nullptr, &present_set_layout_) != VK_SUCCESS) return false;
  }
  if (!present_layout_) {
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &present_set_layout_;
    if (vk_.vkCreatePipelineLayout(dev, &layout_info, nullptr, &present_layout_) != VK_SUCCESS) return false;
  }
  const VkShaderModule fs = CreateShaderModule(EShLangFragment, kPresentShader);
  if (fs) present_pipeline_ = FullscreenPipeline(fs, kGuestOutputFormat, present_layout_);
  return present_pipeline_ != VK_NULL_HANDLE;
}

bool Renderer::Present(uint32_t address, VkImage target, VkImageView target_view, uint32_t width, uint32_t height,
                       const uint32_t* gamma_ramp) {
  auto it = texture_at_.find(address);
  if (it == texture_at_.end()) it = texture_at_.find(last_resolve_address_);
  if (it == texture_at_.end() || !InitPresent()) return false;
  const Image& source = *it->second;

  BeginCommands(kGpuPresent);
  EndRendering();
  const VkDeviceSize ubo_align = device_->properties().minUniformBufferOffsetAlignment;
  VkDeviceSize offset;
  uint8_t* ramp = Upload(kRampBytes, ubo_align, offset);
  if (!ramp || transient_sets_ + 1 > kTransientSetCount) {
    Flush();
    BeginCommands(kGpuPresent);
    ramp = Upload(kRampBytes, ubo_align, offset);
    if (!ramp) return false;
  }
  std::memcpy(ramp, gamma_ramp, kRampBytes);
  const VkDescriptorSet set = AllocateSet(present_set_layout_);
  if (!set) return false;
  const VkDescriptorImageInfo image_info{sampler_, source.view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo buffer_info{ring_.buffer, offset, kRampBytes};
  const VkWriteDescriptorSet writes[2] = {
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       &image_info, nullptr, nullptr},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr,
       &buffer_info, nullptr},
  };
  vk_.vkUpdateDescriptorSets(device_->device(), 2, writes, 0, nullptr);

  // The presenter reads the previous contents in fragment shaders; everything is overwritten, so the old layout
  // is discarded.
  Barrier();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = target;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk_.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = target_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {width, height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vk_.vkCmdBeginRendering(cmd_, &rendering);
  vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
  bound_pipeline_ = present_pipeline_;
  vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout_, 0, 1, &set, 0, nullptr);
  const VkViewport vp{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  vk_.vkCmdSetViewport(cmd_, 0, 1, &vp);
  vk_.vkCmdSetScissor(cmd_, 0, 1, &rendering.renderArea);
  vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
  vk_.vkCmdEndRendering(cmd_);

  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vk_.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
  Flush();
  ++stats_.presents;
  return true;
}

}  // namespace bots::gfx::vk
