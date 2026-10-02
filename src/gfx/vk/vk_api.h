// Vulkan device functions the native renderer uses, loaded from the SDK's device (its own table only has
// what the emulator needs).
#pragma once

#include <string>
#include <type_traits>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/device.h>

namespace bots::gfx::vk {

#define BOTS_VK_DEVICE_FUNCTIONS(X)  \
  X(vkAllocateCommandBuffers)        \
  X(vkAllocateDescriptorSets)        \
  X(vkBeginCommandBuffer)            \
  X(vkCmdBeginRendering)             \
  X(vkCmdBindDescriptorSets)         \
  X(vkCmdBindIndexBuffer)            \
  X(vkCmdBindPipeline)               \
  X(vkCmdBlitImage)                  \
  X(vkCmdClearAttachments)           \
  X(vkCmdClearColorImage)            \
  X(vkCmdCopyBuffer)                 \
  X(vkCmdCopyBufferToImage)          \
  X(vkCmdCopyImage)                  \
  X(vkCmdCopyImageToBuffer)          \
  X(vkCmdDraw)                       \
  X(vkCmdDrawIndexed)                \
  X(vkCmdEndRendering)               \
  X(vkCmdPipelineBarrier)            \
  X(vkCmdPushConstants)              \
  X(vkCmdResetQueryPool)             \
  X(vkCmdSetBlendConstants)          \
  X(vkCmdSetDepthBias)               \
  X(vkCmdSetScissor)                 \
  X(vkCmdSetStencilCompareMask)      \
  X(vkCmdSetStencilReference)        \
  X(vkCmdSetStencilWriteMask)        \
  X(vkCmdSetViewport)                \
  X(vkCmdWriteTimestamp)             \
  X(vkCreateCommandPool)             \
  X(vkCreateDescriptorPool)          \
  X(vkCreateDescriptorSetLayout)     \
  X(vkCreateFence)                   \
  X(vkCreateGraphicsPipelines)       \
  X(vkCreateImageView)               \
  X(vkCreatePipelineLayout)          \
  X(vkCreateQueryPool)               \
  X(vkCreateSampler)                 \
  X(vkCreateShaderModule)            \
  X(vkDestroyCommandPool)            \
  X(vkDestroyDescriptorPool)         \
  X(vkDestroyDescriptorSetLayout)    \
  X(vkDestroyFence)                  \
  X(vkDestroyImageView)              \
  X(vkDestroyPipeline)               \
  X(vkDestroyPipelineLayout)         \
  X(vkDestroyQueryPool)              \
  X(vkDestroySampler)                \
  X(vkDestroyShaderModule)           \
  X(vkDeviceWaitIdle)                \
  X(vkResetDescriptorPool)           \
  X(vkUpdateDescriptorSets)          \
  X(vkEndCommandBuffer)              \
  X(vkGetQueryPoolResults)           \
  X(vkQueueSubmit)                   \
  X(vkResetCommandPool)              \
  X(vkResetFences)                   \
  X(vkWaitForFences)

struct Api {
#define BOTS_VK_DECLARE(name) PFN_##name name = nullptr;
  BOTS_VK_DEVICE_FUNCTIONS(BOTS_VK_DECLARE)
#undef BOTS_VK_DECLARE
  PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;

  // False if any function is missing.
  bool Load(const rex::ui::vulkan::VulkanDevice& device) {
    const auto& ifn = device.vulkan_instance()->functions();
    bool ok = true;
    auto load = [&](auto& fn, const char* name) {
      auto p = ifn.vkGetDeviceProcAddr(device.device(), name);
      // Dynamic rendering is core in 1.3; 1.2 drivers only have the KHR names.
      if (!p) p = ifn.vkGetDeviceProcAddr(device.device(), (std::string(name) + "KHR").c_str());
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
      ok = ok && p;
    };
#define BOTS_VK_LOAD(name) load(name, #name);
    BOTS_VK_DEVICE_FUNCTIONS(BOTS_VK_LOAD)
#undef BOTS_VK_LOAD
    vkGetPhysicalDeviceFormatProperties = ifn.vkGetPhysicalDeviceFormatProperties;
    return ok && vkGetPhysicalDeviceFormatProperties;
  }
};

}  // namespace bots::gfx::vk
