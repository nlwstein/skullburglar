// Renderer: native present. The front buffer goes through the guest's 256-entry gamma ramp into the presenter's
// guest output image, as the emulated swap does for 8_8_8_8 front buffers.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <glslang/Public/ShaderLang.h>
#include <rex/logging.h>

#include "renderer.h"
#include "draw_dump.h"
#include "../../instr_state.h"

namespace bots::gfx::vk {
namespace {

// std140 packs the 256 ramp entries as 64 uvec4s.
constexpr const char* kPresentShader = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2DArray source;
layout(set = 0, binding = 1) uniform Gamma { uvec4 ramp[64]; vec4 render_scale; };
layout(location = 0) out vec4 result;

uint Ramp(float v) {
  uint i = uint(round(clamp(v, 0.0, 1.0) * 255.0));
  return ramp[i >> 2][i & 3u];
}

vec3 Tap(vec2 texel, vec2 size) { return textureLod(source, vec3(texel / size, 0.0), 0.0).rgb; }

// Catmull-Rom from 9 bilinear taps: the middle two weights of each axis share one tap.
vec3 CatmullRom(vec2 position, vec2 size) {
  vec2 t1 = floor(position - 0.5) + 0.5;
  vec2 f = position - t1;
  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  vec2 w3 = f * f * (-0.5 + 0.5 * f);
  vec2 w12 = w1 + w2;
  vec2 t0 = t1 - 1.0, t3 = t1 + 2.0, t12 = t1 + w2 / w12;
  return (Tap(vec2(t0.x, t0.y), size) * w0.x + Tap(vec2(t12.x, t0.y), size) * w12.x +
          Tap(vec2(t3.x, t0.y), size) * w3.x) * w0.y +
         (Tap(vec2(t0.x, t12.y), size) * w0.x + Tap(vec2(t12.x, t12.y), size) * w12.x +
          Tap(vec2(t3.x, t12.y), size) * w3.x) * w12.y +
         (Tap(vec2(t0.x, t3.y), size) * w0.x + Tap(vec2(t12.x, t3.y), size) * w12.x +
          Tap(vec2(t3.x, t3.y), size) * w3.x) * w3.y;
}

void main() {
  // Output pixel centres -> source texels (the front buffer is render_scale.x of the output's size; 1 above render
  // scale 1, where the output is the rendered size). At 1 that is each texel's centre, read as is; below 1 it
  // upscales with Catmull-Rom (render_scale.z 1) or bilinearly.
  vec2 size = vec2(textureSize(source, 0).xy);
  vec2 position = gl_FragCoord.xy * render_scale.x;
  vec3 c = render_scale.z != 0.0 ? CatmullRom(position, size)
                                 : textureLod(source, vec3(position / size, 0.0), 0.0).rgb;
  result = vec4(float((Ramp(c.r) >> 20) & 0x3FFu), float((Ramp(c.g) >> 10) & 0x3FFu), float(Ramp(c.b) & 0x3FFu),
                1023.0) / 1023.0;
}
)";

// BOTS_CAPTURE_BURST="<sec>:<count>[,<sec>:<count>...]": from <sec> seconds after the first present, copy the next
// <count> front buffers (what each present shows, before the gamma ramp and upscale) to a host buffer, one per frame,
// then write them as <BOTS_CAPTURE_BURST_DIR>/b<burst>-<frame>.ppm (default dir "burst") with a sidecar
// b<burst>.tsv (wall ms, presents, n1 frames drawn, marker and scene camera sequences, scene frame). The GPU copies
// cost a few hundred microseconds per frame; the buffer is made when a burst starts and written out after it ends,
// so a burst stalls once at each end. Unset: one predictable branch per present.
// The test instrumentation's `burst <dir> <count>` (instr_state.h) asks for one more burst, starting now.
uint32_t g_instr_burst_seq = 0;  // the last request taken

struct Burst {
  struct Window {
    double at;
    uint32_t count;
    uint32_t every_ms = 0;  // 0: consecutive frames; else a trace (instr_state.h burst_every_ms)
  };
  std::vector<Window> windows;
  size_t next = 0;
  std::string dir;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), burst_start;
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  uint8_t* map = nullptr;
  VkDeviceSize frame_bytes = 0;
  uint32_t w = 0, h = 0, count = 0, taken = 0, number = 0, every_ms = 0;
  std::chrono::steady_clock::time_point last_copy{}, last_present{};
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint64_t first_drawn = 0;
  std::string lines;
};

void BurstToRgb(VkFormat f, const uint8_t* src, size_t texels, std::vector<uint8_t>& rgb) {
  rgb.resize(texels * 3);
  for (size_t i = 0; i < texels; ++i) {
    uint32_t p;
    std::memcpy(&p, src + i * 4, 4);
    uint8_t* o = &rgb[i * 3];
    if (f == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
      for (int k = 0; k < 3; ++k) o[k] = uint8_t(((p >> (10 * k)) & 0x3FF) >> 2);
    } else {
      for (int k = 0; k < 3; ++k) o[k] = uint8_t(p >> (8 * k));
      if (f == VK_FORMAT_B8G8R8A8_UNORM) std::swap(o[0], o[2]);
    }
  }
}

constexpr VkFormat kGuestOutputFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
constexpr VkDeviceSize kRampBytes = 256 * 4 + 16;  // + render_scale

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
  if (it == texture_at_.end()) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("gfx native: nothing to present at {:08X} (last resolve {:08X})", address, last_resolve_address_);
    }
    return false;
  }
  if (!InitPresent()) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_ERROR("gfx native: couldn't create the present pipeline (layout {}, pipeline layout {})",
                   present_set_layout_ != VK_NULL_HANDLE, present_layout_ != VK_NULL_HANDLE);
    }
    return false;
  }
  const Image& source = *it->second;

  BeginCommands(kGpuPresent);
  EndRendering("present");
  const VkDeviceSize ubo_align = device_->properties().minUniformBufferOffsetAlignment;
  VkDeviceSize offset;
  uint8_t* ramp = Upload(kRampBytes, ubo_align, offset);
  if (!ramp || transient_sets_ + 1 > kTransientSetCount) {
    Flush();
    BeginCommands(kGpuPresent);
    ramp = Upload(kRampBytes, ubo_align, offset);
    if (!ramp) return false;
  }
  std::memcpy(ramp, gamma_ramp, 256 * 4);
  // BOTS_GFX_UPSCALE=bilinear: the plain linear sampler instead of Catmull-Rom below render scale 1.
  static const bool bilinear = [] {
    const char* v = std::getenv("BOTS_GFX_UPSCALE");
    return v && std::strcmp(v, "bilinear") == 0;
  }();
  const float ratio = scale_ / output_scale();  // front buffer texels per output pixel
  const float params[4] = {ratio, ratio, ratio < 1.0f && !bilinear ? 1.0f : 0.0f, 0.0f};
  std::memcpy(ramp + 256 * 4, params, sizeof(params));
  const VkDescriptorSet set = AllocateSet(present_set_layout_);
  if (!set) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_ERROR("gfx native: couldn't allocate the present descriptor set");
    }
    return false;
  }
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
  BeginRendering(rendering, "present");
  vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
  bound_pipeline_ = present_pipeline_;
  dynamic_valid_ = false;
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
  static const bool burst = [] {
    const char* v = std::getenv("BOTS_CAPTURE_BURST");
    return v && *v;
  }();
  InstrState* instr = FindInstrState();
  if (burst || (instr && (burst_ || instr->burst_seq.load(std::memory_order_acquire) != g_instr_burst_seq)))
    CaptureBurst(source);
  Flush();
  ++stats_.presents;
  drawdump::OnPresent();
  return true;
}

void Renderer::CaptureBurst(const Image& source) {
  auto* b = static_cast<Burst*>(burst_);
  if (!b) {
    b = new Burst;
    burst_ = b;
    const char* dir = std::getenv("BOTS_CAPTURE_BURST_DIR");
    b->dir = dir && *dir ? dir : "burst";
    for (const char* p = std::getenv("BOTS_CAPTURE_BURST"); p && *p;) {
      double at = 0;
      unsigned count = 0;
      if (std::sscanf(p, "%lf:%u", &at, &count) == 2 && count) b->windows.push_back({at, count});
      p = std::strchr(p, ',');
      if (p) ++p;
    }
    std::error_code ec;
    std::filesystem::create_directories(b->dir, ec);
    REXLOG_INFO("capture burst: {} bursts to {}", b->windows.size(), b->dir);
  }
  const auto now = std::chrono::steady_clock::now();
  if (InstrState* is = FindInstrState(); is && !b->buffer) {
    const uint32_t seq = is->burst_seq.load(std::memory_order_acquire);
    if (seq != g_instr_burst_seq) {
      g_instr_burst_seq = seq;
      b->dir = is->burst_dir[0] ? is->burst_dir : "burst";
      std::error_code ec;
      std::filesystem::create_directories(b->dir, ec);
      const double at = std::chrono::duration<double>(now - b->start).count();
      b->windows.insert(b->windows.begin() + std::ptrdiff_t(std::min(b->next, b->windows.size())),
                        {at, is->burst_count.load(std::memory_order_relaxed),
                         is->burst_every_ms.load(std::memory_order_relaxed)});
    }
  }
  if (!b->buffer) {
    if (b->next >= b->windows.size() ||
        std::chrono::duration<double>(now - b->start).count() < b->windows[b->next].at)
      return;
    if (source.format != VK_FORMAT_R8G8B8A8_UNORM && source.format != VK_FORMAT_B8G8R8A8_UNORM &&
        source.format != VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
      REXLOG_WARN("capture burst: front buffer format {} not supported", int(source.format));
      b->next = b->windows.size();
      return;
    }
    b->every_ms = b->windows[b->next].every_ms;
    b->count = b->windows[b->next++].count;
    b->w = source.width, b->h = source.height, b->format = source.format;
    b->frame_bytes = VkDeviceSize(b->w) * b->h * 4;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = b->frame_bytes * b->count;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(allocator_, &info, &alloc, &b->buffer, &b->alloc, &mapped) != VK_SUCCESS) {
      REXLOG_ERROR("capture burst: {} MB buffer failed", info.size >> 20);
      b->buffer = VK_NULL_HANDLE;
      return;
    }
    b->map = static_cast<uint8_t*>(mapped.pMappedData);
    b->taken = 0, b->burst_start = now, b->first_drawn = n1_drawn_;
    b->last_copy = {}, b->last_present = now;
    // frame: the copy's index (-1: logged, not copied, trace only); gap_ms: since the previous present; phase: the
    // instrumentation's phase at that moment.
    b->lines = "frame\tms\tpresents\tn1_drawn\tmarker_seq\tview_seq\tscene_cam\tscene_frame\tgap_ms\tphase\n";
    REXLOG_INFO("capture burst {}: {} frames of {}x{}{}", b->number, b->count, b->w, b->h,
                b->every_ms ? ", one every " + std::to_string(b->every_ms) + " ms (trace)" : std::string());
  }
  if (source.width != b->w || source.height != b->h) return;  // a resize mid-burst: skip those frames
  const double gap_ms = std::chrono::duration<double, std::milli>(now - b->last_present).count();
  b->last_present = now;
  char phase[9] = {};
  if (InstrState* is = FindInstrState()) {
    const uint64_t p = is->phase8.load(std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i) phase[i] = char((p >> (8 * i)) & 0xFF);
  }
  const bool copy = !b->every_ms || b->taken == 0 || gap_ms >= 50.0 ||
                    std::chrono::duration<double, std::milli>(now - b->last_copy).count() >= b->every_ms;
  if (!copy) {
    char line[200];
    std::snprintf(line, sizeof(line), "-1\t%.2f\t%llu\t%llu\t%u\t%u\t%u\t%llu\t%.2f\t%s\n",
                  std::chrono::duration<double, std::milli>(now - b->burst_start).count(),
                  (unsigned long long)stats_.presents, (unsigned long long)(n1_drawn_ - b->first_drawn),
                  n1_marker_seq_, n1_view_seq_, n1_scene_cam_, (unsigned long long)n1_scene_frame_, gap_ms,
                  phase[0] ? phase : "-");
    b->lines += line;
    return;
  }
  b->last_copy = now;
  BeginCommands(kGpuOther);
  EndRendering("capture burst");
  Barrier();
  VkBufferImageCopy region{};
  region.bufferOffset = b->frame_bytes * b->taken;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {b->w, b->h, 1};
  vk_.vkCmdCopyImageToBuffer(cmd_, source.image, VK_IMAGE_LAYOUT_GENERAL, b->buffer, 1, &region);
  Barrier();
  char line[200];
  std::snprintf(line, sizeof(line), "%u\t%.2f\t%llu\t%llu\t%u\t%u\t%u\t%llu\t%.2f\t%s\n", b->taken,
                std::chrono::duration<double, std::milli>(now - b->burst_start).count(),
                (unsigned long long)stats_.presents, (unsigned long long)(n1_drawn_ - b->first_drawn), n1_marker_seq_,
                n1_view_seq_, n1_scene_cam_, (unsigned long long)n1_scene_frame_, gap_ms, phase[0] ? phase : "-");
  b->lines += line;
  if (++b->taken < b->count) return;
  // The last frame: wait for the copies, hand the pixels to a writer thread, free the buffer.
  Finish();
  vmaInvalidateAllocation(allocator_, b->alloc, 0, VK_WHOLE_SIZE);
  std::vector<uint8_t> pixels(b->map, b->map + b->frame_bytes * b->count);
  vmaDestroyBuffer(allocator_, b->buffer, b->alloc);
  b->buffer = VK_NULL_HANDLE, b->alloc = nullptr, b->map = nullptr;
  const std::string stem = b->dir + "/b" + std::to_string(b->number++);
  REXLOG_INFO("capture burst: {} frames taken, writing {}-*.ppm", b->count, stem);
  std::thread([stem, pixels = std::move(pixels), lines = b->lines, w = b->w, h = b->h, n = b->count,
               format = b->format] {
    std::vector<uint8_t> rgb;
    for (uint32_t i = 0; i < n; ++i) {
      BurstToRgb(format, pixels.data() + size_t(w) * h * 4 * i, size_t(w) * h, rgb);
      char name[32];
      std::snprintf(name, sizeof(name), "-%03u.ppm", i);
      if (FILE* f = std::fopen((stem + name).c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
      }
    }
    if (FILE* f = std::fopen((stem + ".tsv").c_str(), "wb")) {
      std::fwrite(lines.data(), 1, lines.size(), f);
      std::fclose(f);
    }
    REXLOG_INFO("capture burst: wrote {} frames to {}-*.ppm", n, stem);
  }).detach();
}

void Renderer::DestroyBurst() {
  auto* b = static_cast<Burst*>(burst_);
  if (!b) return;
  if (b->buffer) vmaDestroyBuffer(allocator_, b->buffer, b->alloc);  // after Finish
  delete b;
  burst_ = nullptr;
}

}  // namespace bots::gfx::vk
