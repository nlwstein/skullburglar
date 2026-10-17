// The OpenXR side of headset mode (vr.h). Everything here was worked out against a Quest 3's runtime; the parts
// that are not obvious are commented where they bite.
#include "vr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

// Off by default: headset mode is shelved until the pipeline warm-up stops making it look broken (the game
// builds every pipeline on demand inside the frame in a headset, half a second each on an Adreno 740, so it
// stutters and stalls until they are all warm). Opt in per run with `--vr=on`, or per device with `vr=on` in
// bots.toml.
REXCVAR_DEFINE_STRING(vr, "off", "bots/VR",
                      "Headset mode: off (the default - flat), auto (on when a headset with an OpenXR runtime is "
                      "attached), on");

#if REX_PLATFORM_ANDROID

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN

#include <dlfcn.h>
#include <jni.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

namespace bots::gfx::vr {
namespace {

// The flat screen the game is drawn on. Sized to the game's shape (a 16:9 frame gives 1600x900).
constexpr uint32_t kQuadTextureWidth = 1600;
constexpr float kQuadWidthMetres = 2.6f;
constexpr float kQuadDistanceMetres = 2.9f;

std::string ResultName(XrResult result) {
  char buffer[64];
  switch (result) {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
    case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
    case XR_SESSION_NOT_FOCUSED: return "XR_SESSION_NOT_FOCUSED";
    case XR_FRAME_DISCARDED: return "XR_FRAME_DISCARDED";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
    case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
    case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
    case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
    default:
      snprintf(buffer, sizeof buffer, "XrResult(%d)", int(result));
      return buffer;
  }
}

// The app-side state. One headset, one session: the game has a single present path.
void* loader_ = nullptr;
JavaVM* java_vm_ = nullptr;
jobject activity_ = nullptr;  // global ref, the application context the loader wants

PFN_xrGetInstanceProcAddr get_proc_ = nullptr;
PFN_xrCreateInstance create_instance_ = nullptr;
PFN_xrGetSystem get_system_ = nullptr;
PFN_xrGetInstanceProperties get_instance_properties_ = nullptr;
PFN_xrGetVulkanGraphicsRequirements2KHR get_vulkan_requirements_ = nullptr;
PFN_xrCreateVulkanInstanceKHR create_vulkan_instance_ = nullptr;
PFN_xrGetVulkanGraphicsDevice2KHR get_vulkan_device_ = nullptr;
PFN_xrCreateVulkanDeviceKHR create_vulkan_device_ = nullptr;
PFN_xrCreateSession create_session_ = nullptr;
PFN_xrDestroySession destroy_session_ = nullptr;
PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats_ = nullptr;
PFN_xrCreateSwapchain create_swapchain_ = nullptr;
PFN_xrDestroySwapchain destroy_swapchain_ = nullptr;
PFN_xrEnumerateSwapchainImages enumerate_swapchain_images_ = nullptr;
PFN_xrAcquireSwapchainImage acquire_swapchain_image_ = nullptr;
PFN_xrWaitSwapchainImage wait_swapchain_image_ = nullptr;
PFN_xrReleaseSwapchainImage release_swapchain_image_ = nullptr;
PFN_xrCreateReferenceSpace create_reference_space_ = nullptr;
PFN_xrPollEvent poll_event_ = nullptr;
PFN_xrBeginSession begin_session_ = nullptr;
PFN_xrEndSession end_session_ = nullptr;
PFN_xrWaitFrame wait_frame_ = nullptr;
PFN_xrBeginFrame begin_frame_ = nullptr;
PFN_xrEndFrame end_frame_ = nullptr;

bool use_vr_ = false;     // Init() decided the game runs in a headset
bool session_created_ = false;
bool session_running_ = false;  // xrBeginSession has run: the headset is on someone's head
bool session_lost_ = false;     // the runtime is taking the session away; stay on the flat path
bool frame_open_ = false;       // between xrBeginFrame and xrEndFrame
bool frame_submitted_ = false;  // the compositor has taken a frame with the quad
bool frame_failed_ = false;     // the compositor has refused one; reported once
int32_t acquired_image_ = -1;
XrTime frame_display_time_ = 0;  // xrWaitFrame's prediction, which xrEndFrame has to be told

// Vulkan, as the runtime created it.
VkInstance vk_instance_ = VK_NULL_HANDLE;
VkDevice vk_device_ = VK_NULL_HANDLE;
VkQueue vk_queue_ = VK_NULL_HANDLE;

XrInstance instance_ = XR_NULL_HANDLE;
XrSystemId system_ = XR_NULL_SYSTEM_ID;
XrSession session_ = XR_NULL_HANDLE;
XrSpace space_ = XR_NULL_HANDLE;
XrSwapchain swapchain_ = XR_NULL_HANDLE;
VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
uint32_t swapchain_width_ = 0, swapchain_height_ = 0;
std::vector<XrSwapchainImageVulkan2KHR> swapchain_images_;
std::vector<VkImageView> swapchain_views_;

// The Vulkan loader, for the two entry points OpenXR does not hand out: vkCreateInstance (the fallback when the
// runtime will not make the instance) and vkGetDeviceQueue (the session's binding wants the queue itself).
void* VulkanLoader() {
  static void* loader = [] {
    void* handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return handle ? handle : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  }();
  return loader;
}

// The Vulkan entry points the session needs that are not part of the XR structs: the OpenXR Vulkan structs name
// pfnGetInstanceProcAddr as a Vulkan one, and it is, but its type differs by whether the Vulkan headers were
// included with prototypes (ours are not: rex/ui/vulkan/api.h asks for VK_NO_PROTOTYPES).
//
// It has to be the Vulkan loader's function and not the OpenXR loader's xrGetInstanceProcAddr, which fits the
// pointer type but not the meaning: the runtime calls it with a VkInstance, and the OpenXR loader would read that
// as an XrInstance and crash (seen on a Quest 3: SIGSEGV inside libopenxr_loader.so, called from libvrapiimpl.so
// under xrCreateVulkanInstanceKHR).
PFN_vkGetInstanceProcAddr VulkanGetInstanceProcAddr() {
  return VulkanLoader() ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(VulkanLoader(), "vkGetInstanceProcAddr"))
                        : nullptr;
}

// Device entry points the Vulkan loader has to hand over (the SDK's device keeps its own table, but the swapchain
// images are ours, not its).
template <typename T>
T DeviceProc(const char* name) {
  auto get_device_proc = VulkanLoader()
                              ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(VulkanLoader(), "vkGetDeviceProcAddr"))
                              : nullptr;
  return get_device_proc && vk_device_ ? reinterpret_cast<T>(get_device_proc(vk_device_, name)) : nullptr;
}

bool GetProc(const char* name, PFN_xrVoidFunction* out) {
  if (!get_proc_) return false;
  if (XR_FAILED(get_proc_(instance_, name, out)) || !*out) {
    *out = nullptr;
    return false;
  }
  return true;
}

template <typename T>
bool GetProc(const char* name, T* out) {
  return GetProc(name, reinterpret_cast<PFN_xrVoidFunction*>(out));
}

// The loader has to be told where the app's Java side is before anything else, and the one bundled in the APK is
// the only one an app can dlopen (Meta's copy in /system_ext is outside the app's linker namespace).
bool LoadLoader() {
  loader_ = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
  if (!loader_) {
    REXLOG_WARN("vr: no OpenXR loader bundled ({})", dlerror() ? dlerror() : "dlopen failed");
    return false;
  }
  get_proc_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(loader_, "xrGetInstanceProcAddr"));
  if (!get_proc_) {
    REXLOG_WARN("vr: the OpenXR loader has no xrGetInstanceProcAddr");
    return false;
  }
  PFN_xrInitializeLoaderKHR initialize = nullptr;
  if (get_proc_(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initialize)) ==
          XR_SUCCESS &&
      initialize) {
    XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    info.applicationVM = java_vm_;
    info.applicationContext = activity_;
    const XrResult result = initialize(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info));
    if (XR_FAILED(result)) {
      REXLOG_WARN("vr: xrInitializeLoaderKHR: {}", ResultName(result));
      return false;
    }
  }
  return true;
}

// The runtime owns the Vulkan instance and the device (it returns XR_ERROR_VALIDATION_FAILURE for anything else),
// so these two callbacks are what the SDK's VulkanInstance and VulkanDevice call instead of the plain create
// functions. Both are installed before Init() returns, and the device one finishes the setup: with the device
// there, the session, the reference space and later the swapchain can be made.
VkResult CreateInstanceByRuntime(VkInstanceCreateInfo& create_info, const VkAllocationCallbacks* allocator,
                                 VkInstance* instance_out) {
  XrVulkanInstanceCreateInfoKHR xr_info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xr_info.systemId = system_;
  xr_info.pfnGetInstanceProcAddr = VulkanGetInstanceProcAddr();
  xr_info.vulkanCreateInfo = &create_info;
  xr_info.vulkanAllocator = allocator;
  VkResult vulkan_result = VK_SUCCESS;
  const XrResult result = create_vulkan_instance_(instance_, &xr_info, instance_out, &vulkan_result);
  if (XR_SUCCEEDED(result) && vulkan_result == VK_SUCCESS) {
    vk_instance_ = *instance_out;
    REXLOG_INFO("vr: the OpenXR runtime created the Vulkan instance");
    return VK_SUCCESS;
  }
  // The runtime names the violation in logcat (RuntimeTelemetryThread), which is more useful than this line. The
  // game still has to start, so make the instance the ordinary way and stay flat: no XR-created instance means
  // xrCreateSession would be refused anyway.
  REXLOG_ERROR("vr: xrCreateVulkanInstanceKHR: {} (VkResult {}); the game runs flat", ResultName(result),
               int(vulkan_result));
  use_vr_ = false;
  auto create_instance = VulkanLoader()
                             ? reinterpret_cast<PFN_vkCreateInstance>(dlsym(VulkanLoader(), "vkCreateInstance"))
                             : nullptr;
  if (!create_instance) return VK_ERROR_INITIALIZATION_FAILED;
  return create_instance(&create_info, allocator, instance_out);
}

bool CreateSessionByRuntime(VkPhysicalDevice physical_device, uint32_t queue_family) {
  XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  binding.instance = vk_instance_;
  binding.physicalDevice = physical_device;
  binding.device = vk_device_;
  binding.queueFamilyIndex = queue_family;
  binding.queueIndex = 0;
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &binding;
  session_info.systemId = system_;
  const XrResult result = create_session_(instance_, &session_info, &session_);
  if (XR_FAILED(result)) {
    REXLOG_ERROR("vr: xrCreateSession: {} (see the RuntimeTelemetryThread lines in logcat for the reason)",
                 ResultName(result));
    return false;
  }
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;  // the quad stays put in the room
  space_info.poseInReferenceSpace.orientation.w = 1.0f;
  const XrResult space_result = create_reference_space_(session_, &space_info, &space_);
  if (XR_FAILED(space_result)) {
    REXLOG_ERROR("vr: xrCreateReferenceSpace: {}", ResultName(space_result));
    return false;
  }
  session_created_ = true;
  REXLOG_INFO("vr: session created");
  return true;
}

VkResult CreateDeviceByRuntime(VkPhysicalDevice physical_device, VkDeviceCreateInfo& create_info,
                               const VkAllocationCallbacks* allocator, VkDevice* device_out) {
  XrVulkanGraphicsDeviceGetInfoKHR device_get_info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  device_get_info.systemId = system_;
  device_get_info.vulkanInstance = vk_instance_;
  VkPhysicalDevice runtime_device = VK_NULL_HANDLE;
  const XrResult device_result = get_vulkan_device_(instance_, &device_get_info, &runtime_device);
  if (XR_FAILED(device_result) || runtime_device == VK_NULL_HANDLE) {
    REXLOG_ERROR("vr: xrGetVulkanGraphicsDevice2KHR: {}", ResultName(device_result));
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (runtime_device != physical_device) {
    // The SDK picked its own physical device before we got here; the binding has to name the runtime's.
    REXLOG_ERROR("vr: the runtime's Vulkan device is not the one the renderer picked");
  }
  XrVulkanDeviceCreateInfoKHR xr_info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xr_info.systemId = system_;
  xr_info.pfnGetInstanceProcAddr = VulkanGetInstanceProcAddr();
  xr_info.vulkanPhysicalDevice = runtime_device;
  xr_info.vulkanCreateInfo = &create_info;
  xr_info.vulkanAllocator = allocator;
  VkResult vulkan_result = VK_SUCCESS;
  const XrResult result = create_vulkan_device_(instance_, &xr_info, device_out, &vulkan_result);
  if (XR_FAILED(result) || vulkan_result != VK_SUCCESS) {
    REXLOG_ERROR("vr: xrCreateVulkanDeviceKHR: {} (VkResult {})", ResultName(result), int(vulkan_result));
    return vulkan_result != VK_SUCCESS ? vulkan_result : VK_ERROR_INITIALIZATION_FAILED;
  }
  vk_device_ = *device_out;

  // The session's binding wants the queue, and the SDK's create info is the only place its family is named.
  uint32_t queue_family = 0;
  if (create_info.queueCreateInfoCount) queue_family = create_info.pQueueCreateInfos[0].queueFamilyIndex;
  auto get_device_queue = VulkanLoader()
                              ? reinterpret_cast<PFN_vkGetDeviceQueue>(dlsym(VulkanLoader(), "vkGetDeviceQueue"))
                              : nullptr;
  REXLOG_INFO("vr: the OpenXR runtime created the Vulkan device (queue family {})", queue_family);

  // Past this point the device exists, so a failure is not fatal to the game: it runs flat rather than not at
  // all. The instance and device the runtime made are ordinary Vulkan objects, so the flat path works on them.
  if (!get_device_queue || !CreateSessionByRuntime(runtime_device, queue_family)) {
    REXLOG_WARN("vr: no headset session; the game runs flat");
    use_vr_ = false;
  }
  return VK_SUCCESS;
}

// Which of the runtime's swapchain formats to draw into. The present shader can write most formats unchanged;
// the _SRGB ones it has to linearise for first (they get encoded on the way in and decoded by the compositor),
// so prefer one that needs nothing, and fall back through the alpha-first and BGRA spellings.
VkFormat PickSwapchainFormat(const std::vector<int64_t>& formats) {
  const VkFormat wanted[] = {
      VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32,
      VK_FORMAT_R8G8B8A8_SRGB,  VK_FORMAT_B8G8R8A8_SRGB,  VK_FORMAT_A8B8G8R8_SRGB_PACK32,
  };
  for (VkFormat format : wanted) {
    if (std::find(formats.begin(), formats.end(), int64_t(format)) != formats.end()) return format;
  }
  return formats.empty() ? VK_FORMAT_UNDEFINED : VkFormat(formats[0]);
}

bool CreateSwapchain(uint32_t content_width, uint32_t content_height) {
  uint32_t count = 0;
  if (XR_FAILED(enumerate_swapchain_formats_(session_, 0, &count, nullptr)) || !count) {
    REXLOG_ERROR("vr: the runtime offered no swapchain formats");
    return false;
  }
  std::vector<int64_t> formats(count);
  enumerate_swapchain_formats_(session_, count, &count, formats.data());
  swapchain_format_ = PickSwapchainFormat(formats);
  if (swapchain_format_ == VK_FORMAT_UNDEFINED) return false;

  // The quad's texture keeps the game's shape, at a width that stays sharp on a headset panel without costing a
  // mobile GPU more than the frame it holds.
  swapchain_width_ = std::min(kQuadTextureWidth, 4096u);
  const uint32_t aspect_height =
      content_width && content_height
          ? uint32_t(std::lround(double(swapchain_width_) * double(content_height) / double(content_width)))
          : swapchain_width_ * 9 / 16;
  swapchain_height_ = std::max(16u, aspect_height + (aspect_height & 1u));
  swapchain_height_ = std::min(swapchain_height_, 4096u);

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  info.format = swapchain_format_;
  info.sampleCount = 1;
  info.width = swapchain_width_;
  info.height = swapchain_height_;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  XrResult result = create_swapchain_(session_, &info, &swapchain_);
  if (XR_FAILED(result)) {
    REXLOG_ERROR("vr: xrCreateSwapchain ({}x{}, format {}): {}", swapchain_width_, swapchain_height_,
                 int(swapchain_format_), ResultName(result));
    return false;
  }
  uint32_t image_count = 0;
  enumerate_swapchain_images_(swapchain_, 0, &image_count, nullptr);
  if (!image_count) {
    REXLOG_ERROR("vr: the swapchain has no images");
    return false;
  }
  swapchain_images_.resize(image_count);
  for (auto& image : swapchain_images_) image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
  result = enumerate_swapchain_images_(swapchain_, image_count, &image_count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain_images_.data()));
  if (XR_FAILED(result)) {
    REXLOG_ERROR("vr: xrEnumerateSwapchainImages: {}", ResultName(result));
    return false;
  }
  swapchain_images_.resize(image_count);
  swapchain_views_.assign(image_count, VK_NULL_HANDLE);
  // The swapchain images are the runtime's, not the SDK device's, so their views are made with the loader's
  // entry point rather than through the device wrapper.
  const auto create_image_view = DeviceProc<PFN_vkCreateImageView>("vkCreateImageView");
  if (!create_image_view) {
    REXLOG_ERROR("vr: no vkCreateImageView");
    return false;
  }
  for (uint32_t i = 0; i < image_count; ++i) {
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = swapchain_images_[i].image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = swapchain_format_;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (create_image_view(vk_device_, &view_info, nullptr, &swapchain_views_[i]) != VK_SUCCESS) {
      REXLOG_ERROR("vr: could not make a view for swapchain image {}", i);
      return false;
    }
  }
  REXLOG_INFO("vr: quad texture {}x{} x{}, format {}", swapchain_width_, swapchain_height_, image_count,
              int(swapchain_format_));
  return true;
}

// Session states: the headset has to be on someone's head (READY) before frames may be submitted.
void PollEvents() {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (poll_event_ && poll_event_(instance_, &event) == XR_SUCCESS) {
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
      if (changed->state == XR_SESSION_STATE_READY && !session_running_) {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        const XrResult result = begin_session_ ? begin_session_(session_, &begin) : XR_ERROR_FUNCTION_UNSUPPORTED;
        session_running_ = XR_SUCCEEDED(result);
        REXLOG_INFO("vr: session is {}", session_running_ ? "running" : ResultName(result).c_str());
      } else if (changed->state == XR_SESSION_STATE_STOPPING) {
        if (session_running_ && end_session_) end_session_(session_);
        session_running_ = false;
        frame_open_ = false;
        acquired_image_ = -1;
        REXLOG_INFO("vr: the headset went idle");
      } else if (changed->state == XR_SESSION_STATE_EXITING || changed->state == XR_SESSION_STATE_LOSS_PENDING) {
        // Nothing left to draw into: the game carries on on the flat path.
        session_running_ = false;
        session_lost_ = true;
        frame_open_ = false;
        acquired_image_ = -1;
        REXLOG_WARN("vr: the runtime took the session away (state {}); staying flat",
                    int(changed->state));
      }
    }
    event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
  }
}

void DestroySwapchain() {
  if (swapchain_views_.size() && vk_device_) {
    const auto destroy_image_view = DeviceProc<PFN_vkDestroyImageView>("vkDestroyImageView");
    for (VkImageView view : swapchain_views_) {
      if (view && destroy_image_view) destroy_image_view(vk_device_, view, nullptr);
    }
  }
  swapchain_views_.clear();
  swapchain_images_.clear();
  if (swapchain_ && destroy_swapchain_) destroy_swapchain_(swapchain_);
  swapchain_ = XR_NULL_HANDLE;
}

}  // namespace

void SetAndroidActivity(void* java_vm, void* activity) {
  java_vm_ = static_cast<JavaVM*>(java_vm);
  JNIEnv* env = nullptr;
  if (!java_vm_ || !activity || java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return;
  activity_ = env->NewGlobalRef(static_cast<jobject>(activity));
}

bool Init() {
  const std::string mode = rex::cvar::GetFlagByName("vr");
  if (mode == "off") {
    REXLOG_INFO("vr: off");
    return false;
  }
  if (!java_vm_ || !activity_) {
    // Not the Android app (or Java never handed the activity over): nothing to present to.
    if (mode == "on") REXLOG_WARN("vr: on, but no Android activity was handed over");
    return false;
  }
  if (!LoadLoader()) {
    if (mode == "on") REXLOG_WARN("vr: on, but no usable OpenXR loader");
    return false;
  }

  // Both Vulkan bindings: the v1 one answers the extension-list questions, the v2 one creates the instance and
  // the device. A runtime may enable both.
  uint32_t extension_count = 0;
  std::vector<XrExtensionProperties> available;
  PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions = nullptr;
  if (get_proc_(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                reinterpret_cast<PFN_xrVoidFunction*>(&enumerate_extensions)) == XR_SUCCESS &&
      enumerate_extensions && XR_SUCCEEDED(enumerate_extensions(nullptr, 0, &extension_count, nullptr)) &&
      extension_count) {
    available.resize(extension_count);
    for (auto& property : available) property.type = XR_TYPE_EXTENSION_PROPERTIES;
    enumerate_extensions(nullptr, extension_count, &extension_count, available.data());
  }
  auto has_extension = [&available](const char* name) {
    for (const auto& property : available) {
      if (std::strcmp(property.extensionName, name) == 0) return true;
    }
    return false;
  };
  std::vector<const char*> wanted;
  for (const char* name : {"XR_KHR_vulkan_enable", "XR_KHR_vulkan_enable2", "XR_KHR_android_thread_settings"}) {
    if (has_extension(name)) wanted.push_back(name);
  }
  if (!has_extension("XR_KHR_vulkan_enable2")) {
    if (mode == "on") REXLOG_WARN("vr: the runtime does not have the OpenXR Vulkan binding");
    return false;
  }

  XrApplicationInfo application{};
  std::snprintf(application.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "Skullburglar");
  std::snprintf(application.engineName, XR_MAX_ENGINE_NAME_SIZE, "ReXGlue");
  application.apiVersion = XR_API_VERSION_1_0;
  XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
  instance_info.applicationInfo = application;
  instance_info.enabledExtensionCount = uint32_t(wanted.size());
  instance_info.enabledExtensionNames = wanted.data();
  GetProc("xrCreateInstance", &create_instance_);
  XrResult result =
      create_instance_ ? create_instance_(&instance_info, &instance_) : XR_ERROR_FUNCTION_UNSUPPORTED;
  if (XR_FAILED(result)) {
    if (mode == "on") REXLOG_WARN("vr: xrCreateInstance: {}", ResultName(result));
    instance_ = XR_NULL_HANDLE;
    return false;
  }

  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  GetProc("xrGetSystem", &get_system_);
  result = get_system_ ? get_system_(instance_, &system_info, &system_) : XR_ERROR_FUNCTION_UNSUPPORTED;
  if (XR_FAILED(result)) {
    // No headset attached (or none present at all) - the usual answer on a phone or a desktop.
    if (mode == "on") REXLOG_WARN("vr: no headset: {}", ResultName(result));
    instance_ = XR_NULL_HANDLE;
    return false;
  }

  GetProc("xrGetInstanceProperties", &get_instance_properties_);
  XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
  std::string runtime = "unknown runtime";
  if (get_instance_properties_ && XR_SUCCEEDED(get_instance_properties_(instance_, &properties))) {
    runtime = std::string(properties.runtimeName) + " " + std::to_string(XR_VERSION_MAJOR(properties.runtimeVersion)) +
              "." + std::to_string(XR_VERSION_MINOR(properties.runtimeVersion)) + "." +
              std::to_string(XR_VERSION_PATCH(properties.runtimeVersion));
  }

  // Every function is resolved up front: a missing one is a runtime we cannot drive, and finding out here beats
  // finding out mid-frame.
  const bool functions_ok =
      GetProc("xrGetVulkanGraphicsRequirements2KHR", &get_vulkan_requirements_) &&
      GetProc("xrCreateVulkanInstanceKHR", &create_vulkan_instance_) &&
      GetProc("xrGetVulkanGraphicsDevice2KHR", &get_vulkan_device_) &&
      GetProc("xrCreateVulkanDeviceKHR", &create_vulkan_device_) &&
      GetProc("xrCreateSession", &create_session_) && GetProc("xrDestroySession", &destroy_session_) &&
      GetProc("xrEnumerateSwapchainFormats", &enumerate_swapchain_formats_) &&
      GetProc("xrCreateSwapchain", &create_swapchain_) && GetProc("xrDestroySwapchain", &destroy_swapchain_) &&
      GetProc("xrEnumerateSwapchainImages", &enumerate_swapchain_images_) &&
      GetProc("xrAcquireSwapchainImage", &acquire_swapchain_image_) &&
      GetProc("xrWaitSwapchainImage", &wait_swapchain_image_) &&
      GetProc("xrReleaseSwapchainImage", &release_swapchain_image_) &&
      GetProc("xrCreateReferenceSpace", &create_reference_space_) && GetProc("xrPollEvent", &poll_event_) &&
      GetProc("xrBeginSession", &begin_session_) && GetProc("xrEndSession", &end_session_) &&
      GetProc("xrWaitFrame", &wait_frame_) && GetProc("xrBeginFrame", &begin_frame_) &&
      GetProc("xrEndFrame", &end_frame_);
  if (!functions_ok) {
    REXLOG_WARN("vr: the runtime is missing one of the functions the game drives it with");
    instance_ = XR_NULL_HANDLE;
    return false;
  }

  // The spec wants this before the Vulkan instance exists, and the runtime checks it was called.
  XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  result = get_vulkan_requirements_(instance_, system_, &requirements);
  if (XR_FAILED(result)) {
    REXLOG_WARN("vr: xrGetVulkanGraphicsRequirements2KHR: {}", ResultName(result));
    instance_ = XR_NULL_HANDLE;
    return false;
  }

  REXLOG_INFO("vr: headset mode on, runtime {} (Vulkan {}.{}.{} required)", runtime,
              XR_VERSION_MAJOR(requirements.minApiVersionSupported),
              XR_VERSION_MINOR(requirements.minApiVersionSupported),
              XR_VERSION_PATCH(requirements.minApiVersionSupported));

  // From here the runtime creates the game's Vulkan instance and device.
  rex::ui::vulkan::VulkanInstance::SetExternalCreateCallback(CreateInstanceByRuntime);
  rex::ui::vulkan::VulkanDevice::SetExternalCreateCallback(CreateDeviceByRuntime);
  use_vr_ = true;
  return true;
}

bool active() { return use_vr_ && !session_lost_; }

Target BeginFrame(uint32_t content_width, uint32_t content_height) {
  Target target;
  if (!active() || !session_created_) return target;
  PollEvents();
  if (!session_running_ || frame_open_) return target;

  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState state{XR_TYPE_FRAME_STATE};
  if (XR_FAILED(wait_frame_(session_, &wait_info, &state))) return target;
  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  if (XR_FAILED(begin_frame_(session_, &begin_info))) return target;
  frame_open_ = true;
  frame_display_time_ = state.predictedDisplayTime;
  if (!state.shouldRender) return target;  // EndFrame() still has to run; the compositor reprojects.

  if (!swapchain_ && !CreateSwapchain(content_width, content_height)) return target;

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  uint32_t index = 0;
  if (XR_FAILED(acquire_swapchain_image_(swapchain_, &acquire_info, &index))) return target;
  XrSwapchainImageWaitInfo wait_image_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_image_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(wait_swapchain_image_(swapchain_, &wait_image_info))) {
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    release_swapchain_image_(swapchain_, &release_info);
    return target;
  }
  acquired_image_ = int32_t(index);
  target.image = swapchain_images_[index].image;
  target.view = swapchain_views_[index];
  target.format = swapchain_format_;
  target.width = swapchain_width_;
  target.height = swapchain_height_;
  return target;
}

void EndFrame() {
  if (!frame_open_) return;
  frame_open_ = false;
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  std::vector<XrCompositionLayerBaseHeader*> layers;
  if (acquired_image_ >= 0) {
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    release_swapchain_image_(swapchain_, &release_info);
    quad.layerFlags = 0;
    quad.space = space_;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = swapchain_;
    quad.subImage.imageRect = {{0, 0}, {int32_t(swapchain_width_), int32_t(swapchain_height_)}};
    quad.subImage.imageArrayIndex = 0;
    quad.pose.orientation.w = 1.0f;
    quad.pose.position = {0.0f, 0.0f, -kQuadDistanceMetres};
    quad.size = {kQuadWidthMetres, kQuadWidthMetres * float(swapchain_height_) / float(swapchain_width_)};
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
    acquired_image_ = -1;
  }
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = frame_display_time_;
  end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end_info.layerCount = uint32_t(layers.size());
  end_info.layers = layers.empty() ? nullptr : layers.data();
  // The runtime says nothing about a healthy frame, so these two lines are the whole visible account of the
  // headset side: one for the first frame the compositor takes, one for the first it refuses.
  const XrResult result = end_frame_(session_, &end_info);
  if (XR_FAILED(result)) {
    if (!frame_failed_) {
      frame_failed_ = true;
      REXLOG_WARN("vr: xrEndFrame: {} ({} layers)", ResultName(result), layers.size());
    }
  } else if (!frame_submitted_ && !layers.empty()) {
    frame_submitted_ = true;
    REXLOG_INFO("vr: the compositor took the first frame ({}x{} quad, format {})", swapchain_width_,
                swapchain_height_, int(swapchain_format_));
  }
}

void Shutdown() {
  if (session_created_) {
    DestroySwapchain();
    if (session_running_ && end_session_) end_session_(session_);
    session_running_ = false;
    if (session_ && destroy_session_) destroy_session_(session_);
    session_ = XR_NULL_HANDLE;
    session_created_ = false;
  }
  use_vr_ = false;
}

}  // namespace bots::gfx::vr

// VrNative.setActivity (tools/android/java), the only way in: the loader is given the activity before anything else
// happens, and the plugin this lives in is the one that later asks for a headset. JNI's native method lookup finds
// it here because VrNative loads this library; the game's own library deliberately does not define it.
extern "C" JNIEXPORT void JNICALL Java_sh_stz_skullburglar_VrNative_setActivity(JNIEnv* env, jclass,
                                                                                jobject activity) {
  JavaVM* vm = nullptr;
  if (env->GetJavaVM(&vm) != JNI_OK || !vm) return;
  bots::gfx::vr::SetAndroidActivity(vm, activity);
}

#else  // !REX_PLATFORM_ANDROID

// A desktop port would present into the app's own window rather than a quad on a headset panel; until then the
// headset path exists only in the Android build, where the runtime owns the surface.
namespace bots::gfx::vr {

void SetAndroidActivity(void*, void*) {}

bool Init() {
  if (rex::cvar::GetFlagByName("vr") == "on") {
    REXLOG_WARN("vr: headset mode is only implemented in the Android build");
  }
  return false;
}

bool active() { return false; }

Target BeginFrame(uint32_t, uint32_t) { return Target{}; }

void EndFrame() {}

void Shutdown() {}

}  // namespace bots::gfx::vr

#endif  // REX_PLATFORM_ANDROID
