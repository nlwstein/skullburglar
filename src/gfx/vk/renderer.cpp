#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace bots::gfx::vk {
namespace {

namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

constexpr uint32_t kEdramTiles = 2048;
constexpr uint32_t kTileWidthSamples = 80, kTileHeightSamples = 16;
constexpr uint32_t kMaxSurfaceHeight = 4096;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;  // D24S8 isn't on AMD (Deck)

VkFormat ColorTargetFormat(uint32_t format) {
  switch (xe::ColorRenderTargetFormat(format)) {
    case xe::ColorRenderTargetFormat::k_8_8_8_8:
    case xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xe::ColorRenderTargetFormat::k_2_10_10_10:
    case xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xe::ColorRenderTargetFormat::k_16_16:
      return VK_FORMAT_R16G16_SNORM;
    case xe::ColorRenderTargetFormat::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case xe::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xe::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xe::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:  // 2_10_10_10_FLOAT (7e3) and the 16-bit float formats
      return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}

// Resolve destination (guest texture) format and signedness (RB_COPY_DEST_INFO copy_dest_number) -> host format.
VkFormat TextureFormat(uint32_t format, bool is_signed) {
  switch (xe::TextureFormat(format)) {
    case xe::TextureFormat::k_8:
      return is_signed ? VK_FORMAT_R8_SNORM : VK_FORMAT_R8_UNORM;
    case xe::TextureFormat::k_8_8_8_8:
    case xe::TextureFormat::k_8_8_8_8_A:
      return is_signed ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
    case xe::TextureFormat::k_2_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xe::TextureFormat::k_10_11_11:
    case xe::TextureFormat::k_11_11_10:
      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case xe::TextureFormat::k_16_16:
      return is_signed ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
    case xe::TextureFormat::k_16_16_16_16:
      return is_signed ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM;
    case xe::TextureFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xe::TextureFormat::k_24_8:
    case xe::TextureFormat::k_24_8_FLOAT:
    case xe::TextureFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xe::TextureFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case xe::TextureFormat::k_32_32_32_32_FLOAT:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}

float Unorm(uint32_t v, uint32_t bits) { return float(v & ((1u << bits) - 1)) / float((1u << bits) - 1); }

// Xenos 7e3: 3-bit exponent (bias 3), 7-bit mantissa, [0, 31.875].
float Float7e3(uint32_t v) {
  const uint32_t e = (v >> 7) & 7, m = v & 0x7F;
  return e ? std::ldexp(1.0f + float(m) / 128.0f, int(e) - 3) : std::ldexp(float(m) / 128.0f, -2);
}

// Xenos 20e4 depth: 4-bit exponent (bias 15), 20-bit mantissa, [0, 2).
float Float20e4(uint32_t v) {
  const uint32_t e = (v >> 20) & 0xF, m = v & 0xFFFFF;
  return e ? std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15) : std::ldexp(float(m) / 1048576.0f, -14);
}

float Half(uint16_t h) {
  const uint32_t s = h >> 15, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  float f = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 0x400), int(e) - 25);
  return s ? -f : f;
}

// RB_COLOR_CLEAR (+ _LO for 64bpp) is the raw EDRAM value of the cleared pixel.
VkClearColorValue ColorClear(uint32_t format, uint32_t lo, uint32_t hi) {
  VkClearColorValue c{};
  auto snorm16 = [](uint32_t v) { return std::max(-1.0f, float(int16_t(v & 0xFFFF)) / 32767.0f); };
  switch (xe::ColorRenderTargetFormat(format)) {
    case xe::ColorRenderTargetFormat::k_8_8_8_8:
    case xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      c.float32[0] = Unorm(lo, 8), c.float32[1] = Unorm(lo >> 8, 8);
      c.float32[2] = Unorm(lo >> 16, 8), c.float32[3] = Unorm(lo >> 24, 8);
      break;
    case xe::ColorRenderTargetFormat::k_2_10_10_10:
    case xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      c.float32[0] = Unorm(lo, 10), c.float32[1] = Unorm(lo >> 10, 10);
      c.float32[2] = Unorm(lo >> 20, 10), c.float32[3] = Unorm(lo >> 30, 2);
      break;
    case xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      c.float32[0] = Float7e3(lo), c.float32[1] = Float7e3(lo >> 10);
      c.float32[2] = Float7e3(lo >> 20), c.float32[3] = Unorm(lo >> 30, 2);
      break;
    case xe::ColorRenderTargetFormat::k_16_16:
      c.float32[0] = snorm16(lo), c.float32[1] = snorm16(lo >> 16);
      break;
    case xe::ColorRenderTargetFormat::k_16_16_16_16:
      c.float32[0] = snorm16(lo), c.float32[1] = snorm16(lo >> 16);
      c.float32[2] = snorm16(hi), c.float32[3] = snorm16(hi >> 16);
      break;
    case xe::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xe::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      c.float32[0] = Half(lo), c.float32[1] = Half(lo >> 16), c.float32[2] = Half(hi), c.float32[3] = Half(hi >> 16);
      break;
    case xe::ColorRenderTargetFormat::k_32_FLOAT:
    case xe::ColorRenderTargetFormat::k_32_32_FLOAT:
      std::memcpy(&c.float32[0], &lo, 4);
      std::memcpy(&c.float32[1], &hi, 4);
      break;
  }
  return c;
}

uint8_t ToByte(float f) { return uint8_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

Renderer::Renderer(const rex::ui::vulkan::VulkanDevice* device, PhysicalMemory memory)
    : device_(device), memory_(std::move(memory)) {
  if (!device_ || !vk_.Load(*device_) || !device_->properties().dynamicRendering) {
    std::fprintf(stderr, "native renderer: Vulkan device lacks a required function or dynamic rendering\n");
    return;
  }
  allocator_ = rex::ui::vulkan::CreateVmaAllocator(device_, false);
  if (!allocator_) return;
  const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0,
                                          device_->queue_family_graphics_compute()};
  const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  for (Submission& submission : submissions_) {
    if (vk_.vkCreateCommandPool(device_->device(), &pool_info, nullptr, &submission.pool) != VK_SUCCESS ||
        vk_.vkCreateFence(device_->device(), &fence_info, nullptr, &submission.fence) != VK_SUCCESS) {
      return;
    }
    const VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                               submission.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk_.vkAllocateCommandBuffers(device_->device(), &cmd_info, &submission.cmd) != VK_SUCCESS) return;
  }
  // Intel's Windows driver (seen on Gen9, UHD 620, 31.0.101.2135) crashes in vkCmdBeginRendering when a pass
  // without a depth attachment follows one with depth in the same command buffer. There every pass gets one: a
  // scratch depth image that is never loaded or stored. BOTS_GFX_DUMMY_DEPTH=0/1 overrides.
  dummy_depth_ = device_->properties().driverID == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS;
  if (const char* v = std::getenv("BOTS_GFX_DUMMY_DEPTH"); v && *v) dummy_depth_ = *v == '1';
  const VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  vk_.vkCreatePipelineCache(device_->device(), &cache_info, nullptr, &pipeline_cache_);
  if (const char* v = std::getenv("BOTS_GPU_TIMING"); v && *v == '1') {
    VkPhysicalDeviceProperties props;
    device_->vulkan_instance()->functions().vkGetPhysicalDeviceProperties(device_->physical_device(), &props);
    timestamp_ns_ = props.limits.timestampPeriod;
    const VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
                                           VK_QUERY_TYPE_TIMESTAMP, kMaxGpuMarks};
    gpu_timing_ = props.limits.timestampComputeAndGraphics;
    for (Submission& submission : submissions_) {
      gpu_timing_ = gpu_timing_ && vk_.vkCreateQueryPool(device_->device(), &query_info, nullptr,
                                                         &submission.timing_pool) == VK_SUCCESS;
    }
  }
  UseSubmission(0);
  ok_ = InitDrawing();
}

void Renderer::BeginRendering(const VkRenderingInfo& info, const char* what) {
  static const bool trace = [] {
    const char* v = std::getenv("BOTS_TRACE_RENDERING");
    return v && *v == '1';
  }();
  if (trace) {
    std::fprintf(stderr, "BeginRendering %s: area %d,%d %ux%u layers %u colors", what, info.renderArea.offset.x,
                 info.renderArea.offset.y, info.renderArea.extent.width, info.renderArea.extent.height,
                 info.layerCount);
    for (uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
      std::fprintf(stderr, " %p", (void*)info.pColorAttachments[i].imageView);
    }
    std::fprintf(stderr, " depth %p stencil %p\n", info.pDepthAttachment ? (void*)info.pDepthAttachment->imageView : nullptr,
                 info.pStencilAttachment ? (void*)info.pStencilAttachment->imageView : nullptr);
    std::fflush(stderr);
  }
  if (dummy_depth_ && !(info.pDepthAttachment && info.pDepthAttachment->imageView)) {
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = DummyDepth(uint32_t(info.renderArea.offset.x) + info.renderArea.extent.width,
                                 uint32_t(info.renderArea.offset.y) + info.renderArea.extent.height);
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo with_depth = info;
    with_depth.pDepthAttachment = with_depth.pStencilAttachment = &depth;
    vk_.vkCmdBeginRendering(cmd_, &with_depth);
    return;
  }
  vk_.vkCmdBeginRendering(cmd_, &info);
}

VkImageView Renderer::DummyDepth(uint32_t width, uint32_t height) {
  if (dummy_depths_.empty() || dummy_depths_.back()->width < width || dummy_depths_.back()->height < height) {
    // Grows rarely (to the largest surface); older ones are kept, as passes still in flight may use them.
    auto image = std::make_unique<Image>();
    const uint32_t w = std::max(width, dummy_depths_.empty() ? 0u : dummy_depths_.back()->width);
    const uint32_t h = std::max(height, dummy_depths_.empty() ? 0u : dummy_depths_.back()->height);
    if (!CreateImage(*image, kDepthFormat, w, h, true)) return VK_NULL_HANDLE;
    dummy_depths_.push_back(std::move(image));
  }
  return dummy_depths_.back()->view;
}

void Renderer::LoadPipelineCache(const std::filesystem::path& path) {
  pipeline_cache_path_ = path;
  std::vector<char> data;
  if (FILE* f = std::fopen(path.string().c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
      data.resize(size_t(size));
      if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    std::fclose(f);
  }
  if (data.empty() || !pipeline_cache_) return;
  const VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, data.size(),
                                       data.data()};
  VkPipelineCache loaded = VK_NULL_HANDLE;
  if (vk_.vkCreatePipelineCache(device_->device(), &info, nullptr, &loaded) != VK_SUCCESS) return;
  vk_.vkMergePipelineCaches(device_->device(), pipeline_cache_, 1, &loaded);
  vk_.vkDestroyPipelineCache(device_->device(), loaded, nullptr);
  std::fprintf(stderr, "native renderer: pipeline cache %s (%zu KB)\n", path.string().c_str(), data.size() / 1024);
}

void Renderer::SavePipelineCache() {
  if (!pipeline_cache_ || pipeline_cache_path_.empty() || stats_.pipelines == pipelines_saved_) return;
  size_t size = 0;
  if (vk_.vkGetPipelineCacheData(device_->device(), pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size) return;
  std::vector<char> data(size);
  if (vk_.vkGetPipelineCacheData(device_->device(), pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
  std::error_code ec;
  std::filesystem::create_directories(pipeline_cache_path_.parent_path(), ec);
  const std::filesystem::path temp = pipeline_cache_path_.string() + ".tmp";
  FILE* f = std::fopen(temp.string().c_str(), "wb");
  if (!f) return;
  const bool written = std::fwrite(data.data(), 1, size, f) == size;
  std::fclose(f);
  if (written) std::filesystem::rename(temp, pipeline_cache_path_, ec);
  pipelines_saved_ = stats_.pipelines;
}

void Renderer::UseSubmission(uint32_t index) {
  current_ = index;
  const Submission& s = submissions_[index];
  pool_ = s.pool, cmd_ = s.cmd, fence_ = s.fence, transient_pool_ = s.transient_pool;
  ring_ = rings_[index];
  ring_used_ = 0;
  transient_sets_ = 0;
}

void Renderer::WaitSubmission(Submission& submission) {
  if (!submission.pending) return;
  {
    ScopedMicros wait(stats_.wait_us);
    vk_.vkWaitForFences(device_->device(), 1, &submission.fence, VK_TRUE, UINT64_MAX);
  }
  vk_.vkResetFences(device_->device(), 1, &submission.fence);
  if (gpu_timing_ && submission.marks.size() >= 2) {
    const uint32_t n = uint32_t(submission.marks.size());
    std::vector<uint64_t> t(n);
    if (vk_.vkGetQueryPoolResults(device_->device(), submission.timing_pool, 0, n, n * 8, t.data(), 8,
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i + 1 < n; ++i) {
        if (submission.marks[i] < std::size(stats_.gpu_ns) && t[i + 1] >= t[i]) {
          const uint64_t ns = uint64_t(double(t[i + 1] - t[i]) * timestamp_ns_);
          stats_.gpu_ns[submission.marks[i]] += ns;
          auto& label = gpu_labels_[submission.labels[i].empty() ? kGpuCatNames[submission.marks[i]]
                                                                 : submission.labels[i]];
          label.first += ns, ++label.second;
        }
      }
    }
  }
  submission.marks.clear();
  submission.labels.clear();
  for (Image& image : submission.retired) DestroyImage(image);
  submission.retired.clear();
  vk_.vkResetCommandPool(device_->device(), submission.pool, 0);
  vk_.vkResetDescriptorPool(device_->device(), submission.transient_pool, 0);
  submission.pending = false;
}

void Renderer::DestroyImage(Image& image) {
  const VkDevice dev = device_->device();
  for (VkImageView view : {image.view, image.depth_view, image.stencil_view}) {
    if (view) vk_.vkDestroyImageView(dev, view, nullptr);
  }
  for (auto& [s, view] : image.swizzled_views) vk_.vkDestroyImageView(dev, view, nullptr);
  if (image.image) vmaDestroyImage(allocator_, image.image, image.allocation);
  image = Image{};
}

void Renderer::Retire(Image& image) {
  retired_.push_back(image);
  image = Image{};
}

void Renderer::ProcessInvalidations() {
  if (!tracker_) return;
  invalidated_.clear();
  tracker_->TakeInvalidated(invalidated_);
  for (const auto& [first, last] : invalidated_) {
    ++stats_.invalidations;
    auto overlaps = [&](uint32_t start, uint32_t end) { return start < end && start <= last && end > first; };
    for (auto& [key, texture] : guest_textures_) {
      for (const auto& [start, end] : texture->ranges) texture->dirty |= overlaps(start, end);
    }
    // A resolve destination the CPU wrote over is no longer what a fetch of that address reads.
    for (auto* map : {&texture_at_, &depth_texture_at_}) {
      for (auto it = map->begin(); it != map->end();) {
        const auto& image = *it->second;
        const uint64_t bytes = uint64_t(image.width) * image.height * 4;  // close enough: pages, not texels
        it = overlaps(it->first, uint32_t(std::min<uint64_t>(it->first + bytes, UINT32_MAX))) ? map->erase(it)
                                                                                                : std::next(it);
      }
    }
  }
}

void Renderer::EndFrame() {
  constexpr uint64_t kUnusedFrames = 600;  // ~10 s at 60 fps
  if (++frame_ % 64) return;
  for (auto it = guest_textures_.begin(); it != guest_textures_.end();) {
    if (frame_ - it->second->last_used > kUnusedFrames) {
      Retire(it->second->image);
      it = guest_textures_.erase(it);
      ++stats_.texture_evictions;
    } else {
      ++it;
    }
  }
}

bool Renderer::UploadGuestMemory(uint32_t start, uint32_t length) {
  BeginCommands(kGpuUpload);
  EndRendering();
  Barrier();
  while (length) {
    const uint32_t chunk = uint32_t(std::min<VkDeviceSize>(length, ring_.size / 4));
    VkDeviceSize offset;
    uint8_t* staging = Upload(chunk, 16, offset);
    if (!staging) {
      Flush();
      BeginCommands(kGpuUpload);
      staging = Upload(chunk, 16, offset);
      if (!staging) return false;
    }
    std::memcpy(staging, memory_(start), chunk);
    const VkBufferCopy region{offset, start, chunk};
    vk_.vkCmdCopyBuffer(cmd_, ring_.buffer, shared_memory_.buffer, 1, &region);
    stats_.uploaded_bytes += chunk;
    start += chunk, length -= chunk;
  }
  Barrier();
  return true;
}

void Renderer::DestroyFrameState() {
  for (auto& [k, image] : surfaces_) DestroyImage(*image);
  for (auto& [k, image] : textures_) DestroyImage(*image);
  for (auto& [k, image] : crops_) DestroyImage(*image);
  for (auto& [k, texture] : guest_textures_) DestroyImage(texture->image);
  for (auto& image : dummy_depths_) DestroyImage(*image);
  dummy_depths_.clear();
  surfaces_.clear();
  textures_.clear();
  crops_.clear();
  guest_textures_.clear();
  texture_at_.clear();
  depth_texture_at_.clear();
}

void Renderer::ResetFrame() {
  Finish();
  if (vk_.vkDeviceWaitIdle) vk_.vkDeviceWaitIdle(device_->device());
  DestroyFrameState();
  open_pass_ = {};
  shared_memory_uploaded_ = false;
  last_resolve_address_ = 0;
  stats_ = {};
}

Renderer::~Renderer() {
  if (!device_) return;
  const VkDevice dev = device_->device();
  if (vk_.vkDeviceWaitIdle) vk_.vkDeviceWaitIdle(dev);
  SavePipelineCache();
  if (pipeline_cache_) vk_.vkDestroyPipelineCache(dev, pipeline_cache_, nullptr);
  DestroyFrameState();
  for (Image& image : retired_) DestroyImage(image);
  for (Submission& submission : submissions_) {
    for (Image& image : submission.retired) DestroyImage(image);
  }
  for (Image* image : {&null_2d_array_, &null_3d_, &null_cube_}) DestroyImage(*image);
  for (auto& [k, sampler] : samplers_) vk_.vkDestroySampler(dev, sampler, nullptr);
  DestroyTransfers();
  DestroyDrawing();
  for (Submission& submission : submissions_) {
    if (submission.fence) vk_.vkDestroyFence(dev, submission.fence, nullptr);
    if (submission.pool) vk_.vkDestroyCommandPool(dev, submission.pool, nullptr);
    if (submission.timing_pool) vk_.vkDestroyQueryPool(dev, submission.timing_pool, nullptr);
  }
  if (allocator_) vmaDestroyAllocator(allocator_);
}

void Renderer::BeginCommands(GpuCat cat) {
  if (!recording_) {
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk_.vkBeginCommandBuffer(cmd_, &begin);
    recording_ = true;
    bound_pipeline_ = VK_NULL_HANDLE;
    if (gpu_timing_) {
      vk_.vkCmdResetQueryPool(cmd_, submissions_[current_].timing_pool, 0, kMaxGpuMarks);
      submissions_[current_].marks.clear();
      submissions_[current_].labels.clear();
      gpu_cat_ = 0xFF;
    }
  }
  GpuMark(cat);
}

void Renderer::GpuMark(GpuCat cat, bool force) {
  if (!gpu_timing_ || !recording_ || (cat == gpu_cat_ && !force)) return;
  Submission& s = submissions_[current_];
  if (s.marks.size() >= kMaxGpuMarks) return;
  vk_.vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.timing_pool, uint32_t(s.marks.size()));
  s.marks.push_back(cat);
  s.labels.emplace_back();
  gpu_cat_ = cat;
}

void Renderer::GpuLabel(GpuCat cat, std::string label) {
  if (!gpu_timing_ || !recording_) return;
  GpuMark(cat, true);
  Submission& s = submissions_[current_];
  if (!s.labels.empty() && s.labels.size() == s.marks.size()) s.labels.back() = std::move(label);
}

std::string Renderer::TakeGpuLabelReport(double frames, size_t top) {
  std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> rows(gpu_labels_.begin(), gpu_labels_.end());
  gpu_labels_.clear();
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  std::string out;
  for (size_t i = 0; i < rows.size() && i < top; ++i) {
    char line[256];
    std::snprintf(line, sizeof(line), "\n  %7.3f ms  x%-6.1f %s", double(rows[i].second.first) / 1e6 / frames,
                  double(rows[i].second.second) / frames, rows[i].first.c_str());
    out += line;
  }
  return out;
}

// Everything stays in GENERAL layout and every operation is separated by a full barrier. Simple and correct;
// finer barriers are P5 work.
bool Renderer::Conflicts(std::span<Image* const> reads, std::span<Image* const> writes) const {
  auto in = [](const std::vector<const Image*>& set, const Image* image) {
    return std::find(set.begin(), set.end(), image) != set.end();
  };
  for (const Image* image : reads) {
    if (image && in(written_since_barrier_, image)) return true;
  }
  for (const Image* image : writes) {
    if (image && (in(written_since_barrier_, image) || in(read_since_barrier_, image))) return true;
  }
  return false;
}

void Renderer::Track(std::span<Image* const> reads, std::span<Image* const> writes) {
  auto add = [](std::vector<const Image*>& set, const Image* image) {
    if (image && std::find(set.begin(), set.end(), image) == set.end()) set.push_back(image);
  };
  for (const Image* image : reads) add(read_since_barrier_, image);
  for (const Image* image : writes) add(written_since_barrier_, image);
}

void Renderer::Access(std::span<Image* const> reads, std::span<Image* const> writes) {
  if (Conflicts(reads, writes)) Barrier();
  Track(reads, writes);
}

void Renderer::Barrier() {
  written_since_barrier_.clear();
  read_since_barrier_.clear();
  ++stats_.barriers;
  const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
  vk_.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                           &barrier, 0, nullptr, 0, nullptr);
}

void Renderer::EndRendering() {
  if (!rendering_) return;
  vk_.vkCmdEndRendering(cmd_);
  rendering_ = false;
}

void Renderer::Flush() {
  if (!recording_) return;
  EndRendering();
  GpuMark(kGpuOther, true);  // closes the last span
  if (ring_used_) vmaFlushAllocation(allocator_, ring_.allocation, 0, ring_used_);
  vk_.vkEndCommandBuffer(cmd_);
  recording_ = false;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd_;
  {
    auto queue = device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
    vk_.vkQueueSubmit(queue.queue(), 1, &submit, fence_);
  }
  ++stats_.flushes;
  Submission& submitted = submissions_[current_];
  submitted.pending = true;
  submitted.retired.insert(submitted.retired.end(), retired_.begin(), retired_.end());
  retired_.clear();
  const uint32_t next = (current_ + 1) % kSubmissions;
  WaitSubmission(submissions_[next]);
  UseSubmission(next);
}

void Renderer::Finish() {
  Flush();
  for (uint32_t i = 1; i <= kSubmissions; ++i) WaitSubmission(submissions_[(current_ + i) % kSubmissions]);
}

bool Renderer::CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, bool depth,
                           VkImageViewType view_type, bool attachment, uint32_t depth_or_layers, uint32_t levels) {
  const bool cube = view_type == VK_IMAGE_VIEW_TYPE_CUBE;
  const bool is_3d = view_type == VK_IMAGE_VIEW_TYPE_3D;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  if (!depth && SignTwin(format) != VK_FORMAT_UNDEFINED) info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  info.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, is_3d ? depth_or_layers : 1};
  info.mipLevels = levels;
  info.arrayLayers = is_3d ? 1 : cube ? std::max(depth_or_layers, 6u) : depth_or_layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (attachment) {
    info.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateImage(allocator_, &info, &alloc, &image.image, &image.allocation, nullptr) != VK_SUCCESS) {
    return false;
  }
  image.format = format;
  image.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  image.width = width;
  image.height = height;
  image.depth = info.extent.depth;
  image.layers = info.arrayLayers;
  image.levels = levels;
  image.view_type = view_type;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = view_type;
  view.format = format;
  view.subresourceRange = {image.aspect, 0, levels, 0, info.arrayLayers};
  if (vk_.vkCreateImageView(device_->device(), &view, nullptr, &image.view) != VK_SUCCESS) return false;
  if (depth) {
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vk_.vkCreateImageView(device_->device(), &view, nullptr, &image.depth_view) != VK_SUCCESS) return false;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (vk_.vkCreateImageView(device_->device(), &view, nullptr, &image.stencil_view) != VK_SUCCESS) return false;
  }

  BeginCommands(kGpuOther);
  EndRendering();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = {image.aspect, 0, levels, 0, info.arrayLayers};  // both aspects of a depth image
  vk_.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
  return true;
}

Renderer::Image* Renderer::Surface(const rg::RegisterFile& regs, bool depth, uint32_t color_index) {
  const auto surface = regs.Get<rg::reg::RB_SURFACE_INFO>();
  uint32_t base, format;
  bool is_64bpp = false;
  if (depth) {
    const auto info = regs.Get<rg::reg::RB_DEPTH_INFO>();
    // Both depth formats are one host format (D32_SFLOAT_S8_UINT); keep a single image per base.
    base = info.depth_base, format = 0;
  } else {
    const auto info = regs.Get<rg::reg::RB_COLOR_INFO>(rg::reg::RB_COLOR_INFO::rt_register_indices[color_index]);
    // Formats that only differ in blending precision share storage; gamma is applied by the shader, so
    // 8_8_8_8_GAMMA holds the same bytes as 8_8_8_8.
    auto storage = xe::GetStorageColorFormat(info.color_format);
    if (storage == xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) storage = xe::ColorRenderTargetFormat::k_8_8_8_8;
    base = info.color_base, format = uint32_t(storage);
    is_64bpp = xe::IsColorRenderTargetFormat64bpp(info.color_format);
  }
  // Multisampled surfaces are rendered supersampled: one host pixel per guest sample. A 4x surface then is the
  // same image as a 1x surface of twice the pitch over the same tiles, which the XDK relies on (it clears an
  // 880-wide depth buffer by drawing a 440-wide 4x one).
  uint32_t samples_x, samples_y;
  SampleGrid(regs, samples_x, samples_y);
  const uint32_t pitch = surface.surface_pitch * samples_x;
  if (!pitch) return nullptr;
  const SurfaceKey key{base, depth, format, pitch};
  auto& slot = surfaces_[key];
  if (slot) return Own(key, slot.get());

  // No height on Xenos: the surface can extend to the end of EDRAM.
  const uint32_t pitch_tiles = ((pitch + kTileWidthSamples - 1) / kTileWidthSamples) << is_64bpp;
  const uint32_t rows = std::max(1u, (kEdramTiles - base % kEdramTiles) / pitch_tiles);
  const uint32_t height = std::min(rows * kTileHeightSamples, kMaxSurfaceHeight);
  auto image = std::make_unique<Image>();
  if (!CreateImage(*image, depth ? kDepthFormat : ColorTargetFormat(format), pitch, height, depth)) return nullptr;
  slot = std::move(image);
  return Own(key, slot.get());
}

void Renderer::SampleGrid(const rg::RegisterFile& regs, uint32_t& x, uint32_t& y) {
  const auto samples = regs.Get<rg::reg::RB_SURFACE_INFO>().msaa_samples;
  x = samples >= xe::MsaaSamples::k4X ? 2 : 1;
  y = samples >= xe::MsaaSamples::k2X ? 2 : 1;
}

// If another format's surface at the same base and pitch was written more recently, takes over its contents.
Renderer::Image* Renderer::Own(const SurfaceKey& key, Image* image) {
  const auto [base, depth, format, pitch] = key;
  if (depth || !IsTransferable(format)) return image;
  Image* newest = nullptr;
  uint32_t newest_format = 0;
  for (auto& [other_key, other] : surfaces_) {
    const auto [other_base, other_depth, other_format, other_pitch] = other_key;
    // Colour surfaces of another format at the same base and pitch, and depth surfaces at the same base (the XDK
    // clears colour tiles with depth-only draws).
    const bool candidate = other_depth ? other_base == base
                                       : other_base == base && other_pitch == pitch && other_format != format &&
                                             IsTransferable(other_format);
    if (!candidate || other->written <= image->written) continue;
    if (!newest || other->written > newest->written) {
      newest = other.get(), newest_format = other_depth ? kTransferDepth : other_format;
    }
  }
  if (newest) {
    if (trace_textures_) {
      std::fprintf(stderr, "transfer: base %X pitch %u fmt %u -> fmt %u (%ux%u -> %ux%u)\n", base, pitch,
                   newest_format, format, newest->width, newest->height, image->width, image->height);
    }
    Transfer(*newest, newest_format, *image, format);
    image->written = newest->written;
  }
  return image;
}

Renderer::Image* Renderer::ResolveTexture(uint32_t address, uint32_t format, uint32_t width, uint32_t height,
                                          bool depth_value, bool is_signed) {
  const TextureKey key{address, format | (depth_value ? 0x100 : 0) | (is_signed ? 0x200 : 0), width, height};
  auto& slot = textures_[key];
  if (!slot) {
    auto image = std::make_unique<Image>();
    // Shaders sample 2D textures as arrays.
    const VkFormat host_format = depth_value ? VK_FORMAT_R32_SFLOAT : TextureFormat(format, is_signed);
    if (!CreateImage(*image, host_format, width, height, false, VK_IMAGE_VIEW_TYPE_2D_ARRAY)) return nullptr;
    slot = std::move(image);
  }
  if (depth_value) {
    depth_texture_at_[address] = slot.get();
  } else {
    texture_at_[address] = slot.get();
    depth_texture_at_.erase(address);
  }
  return slot.get();
}

void Renderer::OnDraw(const rg::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps, const Draw& draw) {
  if (!ok_) return;
  ScopedMicros time(stats_.draw_us);
  if (regs.Get<rg::reg::RB_MODECONTROL>().edram_mode == xe::EdramMode::kCopy) {
    Resolve(regs);
    return;
  }
  IssueDraw(regs, vs, ps, draw);
}

void Renderer::Resolve(const rg::RegisterFile& regs) {
  ++stats_.resolves;
  const auto copy = regs.Get<rg::reg::RB_COPY_CONTROL>();
  const bool depth_source = copy.copy_src_select >= xe::kMaxColorRenderTargets;

  // The rectangle: D3D puts its 3 rect-list vertices in vertex fetch 0 (as the SDK's GetResolveInfo).
  const auto fetch = regs.GetVertexFetch(0);
  const uint8_t* vertex_data = memory_(fetch.address * 4);
  if (fetch.type != xe::FetchConstantType::kVertex || fetch.size != 6 || !vertex_data) {
    ++stats_.dropped_resolves;
    return;
  }
  const float half_pixel =
      regs.Get<rg::reg::PA_SU_VTX_CNTL>().pix_center == xe::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t v[6];
  for (int i = 0; i < 6; ++i) {
    uint32_t bits;
    std::memcpy(&bits, vertex_data + 4 * i, 4);
    float f;
    const uint32_t swapped = xe::GpuSwap(bits, fetch.endian);
    std::memcpy(&f, &swapped, 4);
    v[i] = int32_t(std::lround((f + half_pixel) * 256.0f));  // 16.8 fixed point
  }
  int32_t x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8, y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
  int32_t x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8, y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;
  const auto window_offset = regs.Get<rg::reg::PA_SC_WINDOW_OFFSET>();
  if (regs.Get<rg::reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    x0 += window_offset.window_x_offset, x1 += window_offset.window_x_offset;
    y0 += window_offset.window_y_offset, y1 += window_offset.window_y_offset;
  }
  const auto tl = regs.Get<rg::reg::PA_SC_WINDOW_SCISSOR_TL>();
  const auto br = regs.Get<rg::reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t sx0 = tl.tl_x, sy0 = tl.tl_y, sx1 = br.br_x, sy1 = br.br_y;
  if (!tl.window_offset_disable) {
    sx0 += window_offset.window_x_offset, sx1 += window_offset.window_x_offset;
    sy0 += window_offset.window_y_offset, sy1 += window_offset.window_y_offset;
  }
  x0 = std::clamp(x0, std::max(sx0, 0), std::max(sx1, 0)), x1 = std::clamp(x1, std::max(sx0, 0), std::max(sx1, 0));
  y0 = std::clamp(y0, std::max(sy0, 0), std::max(sy1, 0)), y1 = std::clamp(y1, std::max(sy0, 0), std::max(sy1, 0));
  x0 &= ~7, y0 &= ~7, x1 = (x1 + 7) & ~7, y1 = (y1 + 7) & ~7;  // D3D aligns resolves to 8
  const int32_t pitch = int32_t(regs.Get<rg::reg::RB_SURFACE_INFO>().surface_pitch & ~7u);
  x0 = std::min(x0, pitch), x1 = std::min(x1, pitch);
  if (x0 >= x1 || y0 >= y1) {
    ++stats_.dropped_resolves;
    return;
  }

  BeginCommands(kGpuResolve);
  EndRendering();
  uint32_t samples_x, samples_y;
  SampleGrid(regs, samples_x, samples_y);
  const bool multisampled = samples_x * samples_y > 1;
  Image* source = Surface(regs, depth_source, depth_source ? 0 : copy.copy_src_select);
  if (!source) {
    ++stats_.dropped_resolves;
    return;
  }
  auto clamp_rect = [&](const Image& image, int32_t& w, int32_t& h) {
    w = std::min(x1, int32_t(image.width)) - x0;
    h = std::min(y1, int32_t(image.height)) - y0;
    return w > 0 && h > 0;
  };

  // Copy.
  const auto dest_pitch = regs.Get<rg::reg::RB_COPY_DEST_PITCH>();
  const auto dest_info = regs.Get<rg::reg::RB_COPY_DEST_INFO>();
  const uint32_t dest_address = regs.values[rg::XE_GPU_REG_RB_COPY_DEST_BASE];
  const bool copying = (copy.copy_command == xe::CopyCommand::kRaw || copy.copy_command == xe::CopyCommand::kConvert) &&
                       dest_pitch.copy_dest_pitch && dest_pitch.copy_dest_height;
  if (trace_textures_) {
    const auto color_info = regs.Get<rg::reg::RB_COLOR_INFO>(
        rg::reg::RB_COLOR_INFO::rt_register_indices[depth_source ? 0 : copy.copy_src_select]);
    std::fprintf(stderr,
                 "resolve %llu: %s src %u (surface %ux%u fmt %u) rect %d,%d-%d,%d -> %08X fmt %u %ux%u cmd %u "
                 "bias %d num %u swap %u%s clear %s%s\n",
                 (unsigned long long)stats_.resolves, depth_source ? "depth" : "color", copy.copy_src_select,
                 source->width, source->height, depth_source ? 0u : uint32_t(color_info.color_format), x0, y0, x1, y1,
                 dest_address, uint32_t(dest_info.copy_dest_format), uint32_t(dest_pitch.copy_dest_pitch),
                 uint32_t(dest_pitch.copy_dest_height), uint32_t(copy.copy_command), int(dest_info.copy_dest_exp_bias),
                 uint32_t(dest_info.copy_dest_number), uint32_t(dest_info.copy_dest_swap), multisampled ? " msaa" : "", copy.color_clear_enable ? "color " : "", copy.depth_clear_enable ? "depth" : "");
  }
  if (copying && multisampled) {
    ++stats_.msaa_resolves;  // would need a downsample; none seen in this game yet
  } else if (copying && depth_source) {
    // D3D resolves depth to an 8888 texture holding the raw 24_8 bits; games sample it as 24_8 / 24_8_FLOAT
    // (the depth value) or as 8888 (to restore depth). Keep both.
    const bool float_depth =
        regs.Get<rg::reg::RB_DEPTH_INFO>().depth_format == xe::DepthRenderTargetFormat::kD24FS8;
    Image* raw = ResolveTexture(dest_address, uint32_t(xe::TextureFormat::k_8_8_8_8), dest_pitch.copy_dest_pitch,
                                dest_pitch.copy_dest_height);
    Image* value = ResolveTexture(dest_address, uint32_t(float_depth ? xe::TextureFormat::k_24_8_FLOAT
                                                                     : xe::TextureFormat::k_24_8),
                                  dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height, true);
    int32_t w, h, dw, dh;
    if (raw && value && clamp_rect(*source, w, h) && clamp_rect(*raw, dw, dh)) {
      const VkRect2D rect{{x0, y0}, {uint32_t(std::min(w, dw)), uint32_t(std::min(h, dh))}};
      Transfer(*source, float_depth ? kTransferFloatDepth : kTransferDepth, *raw,
               uint32_t(xe::ColorRenderTargetFormat::k_8_8_8_8), &rect);
      Transfer(*source, kTransferDepth, *value, kTransferDepthValue, &rect);
      raw->written = value->written = ++write_clock_;
      DumpResolve(*value, dest_address);
    } else {
      ++stats_.dropped_resolves;
    }
  } else if (copying) {
    const uint32_t format = uint32_t(dest_info.copy_dest_format);
    const auto number = dest_info.copy_dest_number;
    const bool is_signed = number == xe::SurfaceNumberFormat::kSignedRepeatingFraction ||
                           number == xe::SurfaceNumberFormat::kSignedInteger;
    Image* dest = ResolveTexture(dest_address, format, dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height,
                                 false, is_signed);
    int32_t w, h, dw, dh;
    if (dest && clamp_rect(*source, w, h) && clamp_rect(*dest, dw, dh)) {
      // The resolve converts through the shader into the destination's format and signedness, with the exponent
      // bias (UE3 copies its HDR scene out at 1/8). Fixed-point 16-bit surfaces hold value / 32 on the host. The
      // red/blue swap is not applied: the host image keeps red first and remembers the swap for sampling.
      const auto source_format = xe::GetStorageColorFormat(
          regs.Get<rg::reg::RB_COLOR_INFO>(rg::reg::RB_COLOR_INFO::rt_register_indices[copy.copy_src_select])
              .color_format);
      const bool fixed = source_format == xe::ColorRenderTargetFormat::k_16_16 ||
                         source_format == xe::ColorRenderTargetFormat::k_16_16_16_16;
      const VkRect2D rect{{x0, y0}, {uint32_t(std::min(w, dw)), uint32_t(std::min(h, dh))}};
      Transfer(*source, 0, *dest, kTransferResolve, &rect,
               std::ldexp(1.0f, int(dest_info.copy_dest_exp_bias) + (fixed ? 5 : 0)));
      dest->swapped = dest_info.copy_dest_swap;
      dest->written = ++write_clock_;
      last_resolve_address_ = dest_address;
      DumpResolve(*dest, dest_address);
    } else {
      ++stats_.dropped_resolves;
    }
  }

  // Clears, after the copy, of every sample in the rect.
  x0 *= samples_x, x1 *= samples_x, y0 *= samples_y, y1 *= samples_y;
  auto clear = [&](Image& image, bool depth, const VkClearValue& value) {
    int32_t w, h;
    if (!clamp_rect(image, w, h)) return;
    Image* const target = &image;
    Access({}, {&target, 1});
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = image.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{x0, y0}, {uint32_t(w), uint32_t(h)}};
    image.Use(uint32_t(x0 + w), uint32_t(y0 + h));
    rendering.layerCount = 1;
    if (depth) {
      rendering.pDepthAttachment = rendering.pStencilAttachment = &attachment;
    } else {
      rendering.colorAttachmentCount = 1;
      rendering.pColorAttachments = &attachment;
    }
    BeginRendering(rendering, "clear");
    const VkClearAttachment what{depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                       : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                                 0, value};
    const VkClearRect rect{{{x0, y0}, {uint32_t(w), uint32_t(h)}}, 0, 1};
    vk_.vkCmdClearAttachments(cmd_, 1, &what, 1, &rect);
    vk_.vkCmdEndRendering(cmd_);
    image.written = ++write_clock_;
    ++stats_.clears;
  };
  if (copy.color_clear_enable && !depth_source) {
    const auto info =
        regs.Get<rg::reg::RB_COLOR_INFO>(rg::reg::RB_COLOR_INFO::rt_register_indices[copy.copy_src_select]);
    VkClearValue value;
    value.color = ColorClear(uint32_t(info.color_format), regs.values[rg::XE_GPU_REG_RB_COLOR_CLEAR],
                             regs.values[rg::XE_GPU_REG_RB_COLOR_CLEAR_LO]);
    clear(*source, false, value);
  }
  if (copy.depth_clear_enable) {
    if (Image* depth = Surface(regs, true, 0)) {
      const uint32_t raw = regs.values[rg::XE_GPU_REG_RB_DEPTH_CLEAR];
      const bool float_depth =
          regs.Get<rg::reg::RB_DEPTH_INFO>().depth_format == xe::DepthRenderTargetFormat::kD24FS8;
      VkClearValue value;
      value.depthStencil = {float_depth ? Float20e4(raw >> 8) : float(raw >> 8) / 16777215.0f, raw & 0xFF};
      clear(*depth, true, value);
    }
  }
}

// BOTS_DUMP_RESOLVES=<dir> writes every resolve as <dir>/resolve-<n>-<address>.ppm (debugging; depth resolves
// show the depth value).
void Renderer::DumpResolve(const Image& image, uint32_t address) {
  static const char* dump_dir = std::getenv("BOTS_DUMP_RESOLVES");
  if (!dump_dir || !*dump_dir) return;
  std::vector<uint8_t> rgb;
  uint32_t width, height;
  if (ReadImage(image, rgb, width, height)) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/resolve-%02llu-%08X.ppm", dump_dir, (unsigned long long)stats_.resolves,
                  address);
    if (FILE* f = std::fopen(path, "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", width, height);
      std::fwrite(rgb.data(), 1, rgb.size(), f);
      std::fclose(f);
    }
  }
  BeginCommands(kGpuOther);
}

bool Renderer::ReadTexture(uint32_t address, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
  const auto it = texture_at_.find(address);
  return it != texture_at_.end() && ReadImage(*it->second, rgb, width, height);
}

bool Renderer::ReadImage(const Image& image, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
  uint32_t texel;
  switch (image.format) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UNORM:
      texel = 4;
      break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
      texel = 8;
      break;
    default:
      std::fprintf(stderr, "native renderer: no readback for format %d\n", int(image.format));
      return false;
  }
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = VkDeviceSize(image.width) * image.height * texel;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer buffer;
  VmaAllocation allocation;
  VmaAllocationInfo mapped;
  if (vmaCreateBuffer(allocator_, &info, &alloc, &buffer, &allocation, &mapped) != VK_SUCCESS) return false;

  BeginCommands(kGpuOther);
  Barrier();
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {image.width, image.height, 1};
  vk_.vkCmdCopyImageToBuffer(cmd_, image.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
  Finish();
  vmaInvalidateAllocation(allocator_, allocation, 0, VK_WHOLE_SIZE);

  // BOTS_DUMP_SCALE=<f> scales the values first, to look at HDR targets (debugging).
  static const float scale = [] {
    const char* v = std::getenv("BOTS_DUMP_SCALE");
    return v ? float(std::atof(v)) : 1.0f;
  }();
  // BOTS_DUMP_7E3=1 shows 2_10_10_10 textures as 7e3 (UE3 copies HDR scene colour out through them).
  static const bool as_7e3 = std::getenv("BOTS_DUMP_7E3") != nullptr;
  width = image.width, height = image.height;
  rgb.resize(size_t(width) * height * 3);
  const auto* src = static_cast<const uint8_t*>(mapped.pMappedData);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    float c[3];
    if (image.format == VK_FORMAT_R32_SFLOAT) {
      float v;
      std::memcpy(&v, src + i * 4, 4);
      c[0] = c[1] = c[2] = v;
    } else if (image.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
      uint16_t h[4];
      std::memcpy(h, src + i * 8, 8);
      for (int k = 0; k < 3; ++k) c[k] = Half(h[k]);
    } else if (image.format == VK_FORMAT_R16G16B16A16_UNORM || image.format == VK_FORMAT_R16G16_UNORM) {
      const bool four = image.format == VK_FORMAT_R16G16B16A16_UNORM;
      uint16_t v[4] = {};
      std::memcpy(v, src + i * texel, four ? 8 : 4);
      for (int k = 0; k < 3; ++k) c[k] = (k < 2 || four) ? float(v[k]) / 65535.0f : 0.0f;
    } else if (image.format == VK_FORMAT_R16G16B16A16_SNORM || image.format == VK_FORMAT_R16G16_SNORM) {
      // Guest 16-bit fixed point: -32..32.
      const bool four = image.format == VK_FORMAT_R16G16B16A16_SNORM;
      int16_t v[4] = {};
      std::memcpy(v, src + i * texel, four ? 8 : 4);
      for (int k = 0; k < 3; ++k) c[k] = (k < 2 || four) ? float(v[k]) / 32767.0f * 32.0f : 0.0f;
    } else {
      uint32_t p;
      std::memcpy(&p, src + i * 4, 4);
      if (image.format == VK_FORMAT_R8G8B8A8_UNORM) {
        for (int k = 0; k < 3; ++k) c[k] = Unorm(p >> (8 * k), 8);
      } else if (as_7e3) {
        for (int k = 0; k < 3; ++k) c[k] = rex::graphics::xenos::Float7e3To32((p >> (10 * k)) & 0x3FF);
      } else {
        for (int k = 0; k < 3; ++k) c[k] = Unorm(p >> (10 * k), 10);
      }
    }
    for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = ToByte(c[k] * scale);
  }
  vmaDestroyBuffer(allocator_, buffer, allocation);
  return true;
}

}  // namespace bots::gfx::vk
