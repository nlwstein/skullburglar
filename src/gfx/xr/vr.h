#pragma once
// Headset mode: the game's flat output on a quad layer in front of the player, through OpenXR.
//
// The Quest's runtime refuses a Vulkan instance or device it did not create itself (it says so in logcat), so
// this takes over both through the SDK's external-create callbacks - which is why Init() has to run before the
// graphics provider exists. Nothing here is Quest-specific beyond that: any OpenXR runtime that speaks the
// Vulkan binding works, and a device without a headset leaves the game flat.
//
// Android only for now. Desktop runtimes want a swapchain on the app's own window, not a quad layer on a
// headset panel, so a desktop port would be a different module.
#include <cstdint>
// The SDK's Vulkan entry point, never <vulkan/vulkan.h> directly: it defines VK_ENABLE_BETA_EXTENSIONS before the
// headers, and pulling vulkan_core.h in first would leave the C++ bindings' enum list disagreeing with it.
#include <rex/ui/vulkan/api.h>

namespace bots::gfx::vr {

// Starts the loader with the activity as the application context (XR_KHR_loader_init_android wants both it and
// the JavaVM). Called from the app's Java before any game code runs - the APK bundles the Khronos loader, which
// cannot be reached from an app's linker namespace the way Meta's system copy can be from a shell.
void SetAndroidActivity(void* java_vm, void* activity);

// Decides from the cvar `vr` (off|auto|on, default off) and the device whether the game runs in a headset, and
// prepares for the Vulkan instance and device the runtime will create. False: the game runs flat - always,
// unless the cvar asks for a headset.
bool Init();

// True once the runtime owns the instance, device and session. The present path then draws into the headset
// instead of the window.
bool active();

struct Target {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  bool valid() const { return image != VK_NULL_HANDLE; }
};

// One headset frame, at the game's own swap rate: waits for the runtime to ask for it and takes a swapchain
// image to draw into. An invalid target means the runtime wants no frame this time (or the session is not
// running) - the caller presents to the window instead, and EndFrame() is still required.
//
// `content_width` and `content_height` are what the frame holds (the renderer's output size): the first frame
// sizes the quad's texture to the same shape. Both may be zero once the swapchain exists.
Target BeginFrame(uint32_t content_width, uint32_t content_height);

// Releases the frame's swapchain image, hands the quad to the compositor and ends the frame.
void EndFrame();

// Stops the session and drops everything OpenXR. The Vulkan objects belong to the renderer, not to us.
void Shutdown();

}  // namespace bots::gfx::vr
