// BOTS_GFX_SHADOW=<seconds>: native renderer shadow mode (P3).
//
// Every <seconds> one frame's PM4 is recorded (capture.cpp does the stitching), rendered offscreen by the native
// renderer at that frame's Swap from live guest memory, and compared with what the emulated path presented. One
// log line per sample ("gfx shadow: ..."). The native renderer starts each sample from scratch (no surfaces or
// textures carried over), exactly like bots-replay with a capture, so this is replay without the 300 MB files.
// Like a capture it stalls the frame it samples: a validation tool, not the cutover (P4).
//   BOTS_GFX_SHADOW_DIR=dir   write native + reference PPMs of samples below BOTS_GFX_SHADOW_MIN dB (default 40)
//   BOTS_GFX_SHADOW_LIVE=1    render every frame with one persistent renderer instead, guest memory tracked
//                             (vk/memory_sync.h): surfaces, textures and resolves carry over between frames as they
//                             will after the cutover. Samples still compare every BOTS_GFX_SHADOW seconds.
#include "shadow.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include <rex/logging.h>

#ifdef BOTS_NATIVE_GFX
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include "image_diff.h"
#include "pm4.h"
#include "vk/memory_sync.h"
#include "vk/renderer.h"
#endif

namespace bots::gfx {

namespace {

double ConfiguredInterval() {
  const char* v = std::getenv("BOTS_GFX_SHADOW");
  return v && *v ? std::max(0.0, std::strtod(v, nullptr)) : 0.0;
}

bool ConfiguredLive() {
  const char* v = std::getenv("BOTS_GFX_SHADOW_LIVE");
  return v && *v == '1';
}

}  // namespace

#ifndef BOTS_NATIVE_GFX

double ShadowInterval() {
  static const bool warned = [] {
    if (ConfiguredInterval() > 0 || ConfiguredLive()) {
      REXLOG_WARN("gfx shadow: this build has no native renderer; BOTS_GFX_SHADOW ignored");
    }
    return true;
  }();
  (void)warned;
  return 0;
}
bool ShadowLive() { return ShadowInterval() > 0; }
void ShadowRender(uint64_t, const std::vector<uint8_t>&, bool) {}
void ShadowCompare(uint64_t, const rex::ui::RawImage&, const uint32_t*) {}

#else

namespace {

struct Shadow {
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider;
  std::unique_ptr<vk::Renderer> renderer;
  std::unique_ptr<vk::MemorySync> memory;  // live mode
  std::unique_ptr<Pm4Executor> executor;   // live mode: register state carries over between frames
  uint64_t frames = 0;
  double frame_ms_total = 0;
  bool failed = false;
  // The last rendered sample.
  uint64_t frame = UINT64_MAX;
  std::vector<uint8_t> image;
  uint32_t width = 0, height = 0;
  vk::Renderer::Stats stats;
  double render_ms = 0;
  // Totals.
  uint64_t samples = 0, below = 0;
  double worst = 99;
  std::string dir;
  double min_psnr = 40;
};

Shadow& State() {
  static Shadow* s = [] {
    auto* s = new Shadow;
    if (const char* dir = std::getenv("BOTS_GFX_SHADOW_DIR")) s->dir = dir;
    if (const char* min = std::getenv("BOTS_GFX_SHADOW_MIN")) s->min_psnr = std::strtod(min, nullptr);
    return s;
  }();
  return *s;
}

// Live mode keeps one renderer: a sample reports its own frame's share of the counters.
vk::Renderer::Stats Delta(const vk::Renderer::Stats& after, const vk::Renderer::Stats& before) {
  static_assert(sizeof(vk::Renderer::Stats) % sizeof(uint64_t) == 0);
  constexpr size_t n = sizeof(vk::Renderer::Stats) / sizeof(uint64_t);
  uint64_t a[n], b[n];
  std::memcpy(a, &after, sizeof(a));
  std::memcpy(b, &before, sizeof(b));
  for (size_t i = 0; i < n; ++i) a[i] -= b[i];
  vk::Renderer::Stats delta;
  std::memcpy(&delta, a, sizeof(a));
  return delta;
}

const uint8_t* Physical(uint32_t address) {
  return rex::system::kernel_memory()->TranslatePhysical<const uint8_t*>(address);
}

}  // namespace

double ShadowInterval() {
  static const double interval = ConfiguredInterval();
  return State().failed ? 0 : interval;
}

bool ShadowLive() {
  static const bool live = ConfiguredLive();
  return live && !State().failed;
}

void ShadowRender(uint64_t frame, const std::vector<uint8_t>& pm4, bool sample) {
  Shadow& s = State();
  if (s.failed) return;
  const bool live = ShadowLive();
  if (!s.renderer) {
    // Its own Vulkan device, independent of whichever backend the emulated path presents with.
    s.provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
    if (s.provider) s.renderer = std::make_unique<vk::Renderer>(s.provider->vulkan_device(), Physical);
    if (!s.renderer || !s.renderer->ok()) {
      REXLOG_ERROR("gfx shadow: native renderer setup failed; shadow mode off");
      s.failed = true;
      return;
    }
    if (live) {
      s.memory = std::make_unique<vk::MemorySync>(*rex::system::kernel_memory(), *s.renderer);
      s.renderer->SetMemoryTracker(s.memory.get());
      s.executor = std::make_unique<Pm4Executor>(Physical, s.renderer.get());
    }
    REXLOG_INFO("gfx shadow: native renderer up ({}), comparing every {} s", live ? "every frame" : "samples only",
                ShadowInterval());
  }
  const auto start = std::chrono::steady_clock::now();
  const vk::Renderer::Stats before = live ? s.renderer->stats() : vk::Renderer::Stats{};
  if (live) {
    s.executor->Execute(pm4.data(), pm4.size());
  } else {
    s.renderer->ResetFrame();
    Pm4Executor executor(Physical, s.renderer.get());
    executor.Execute(pm4.data(), pm4.size());
  }
  s.renderer->Flush();
  if (live) s.renderer->EndFrame();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  ++s.frames;
  s.frame_ms_total += ms;
  s.frame = UINT64_MAX;
  if (!s.renderer->ok()) {
    REXLOG_ERROR("gfx shadow: frame {}: native renderer failed; shadow mode off", frame);
    s.failed = true;
    return;
  }
  if (!sample) return;
  s.render_ms = live ? s.frame_ms_total / double(s.frames) : ms;
  s.stats = Delta(s.renderer->stats(), before);
  if (!s.renderer->ReadTexture(s.renderer->last_resolve_address(), s.image, s.width, s.height)) {
    REXLOG_WARN("gfx shadow: frame {}: nothing resolved ({} draws)", frame, s.stats.draws);
    return;
  }
  s.frame = frame;
}

void ShadowCompare(uint64_t frame, const rex::ui::RawImage& reference, const uint32_t* gamma_table) {
  Shadow& s = State();
  if (s.frame != frame || !reference.width) return;
  if (gamma_table) ApplyGammaRamp(s.image, gamma_table);
  const ImageDiff d = CompareImages(s.image, s.width, s.height, reference.data.data(), reference.width,
                                    reference.height, reference.stride, 4);
  ++s.samples;
  s.worst = std::min(s.worst, d.psnr);
  const bool low = d.psnr < s.min_psnr;
  s.below += low;
  const auto& st = s.stats;
  REXLOG_INFO(
      "gfx shadow: frame {}: {:.1f} dB, {:.2f}% off >16, mean {:.2f} | {} draws ({} not rendered), {} resolves "
      "({} msaa), {} failed shaders, {} unsupported prims, {} unsupported textures | {:.0f} ms{} | {} MB uploaded, "
      "{} invalidations, {} texture reloads, {} evictions | {} samples, {} below {:.0f} dB, worst {:.1f}",
      frame, d.psnr, d.bad_percent, d.mean_abs, st.draws, st.skipped_draws, st.resolves, st.msaa_resolves,
      st.failed_shaders, st.unsupported_primitives, st.unsupported_textures, s.render_ms,
      ShadowLive() ? " per frame (avg)" : "", st.uploaded_bytes >> 20, st.invalidations, st.texture_reloads,
      st.texture_evictions, s.samples, s.below, s.min_psnr, s.worst);
  if (low && !s.dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(s.dir, ec);
    const auto base = std::filesystem::path(s.dir) / ("f" + std::to_string(frame));
    WritePpm(base.string() + "-native.ppm", s.image.data(), s.width, s.height, size_t(s.width) * 3, 3);
    WritePpm(base.string() + "-reference.ppm", reference.data.data(), reference.width, reference.height,
             reference.stride, 4);
  }
  s.frame = UINT64_MAX;
}

#endif

}  // namespace bots::gfx
