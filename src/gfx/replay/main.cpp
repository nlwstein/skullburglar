// bots-replay: renders a D3D frame capture offline (native renderer, P2).
//
//   bots-replay CAPDIR [--draws]         per-draw state report
//   bots-replay CAPDIR --render OUT.ppm  render with the native renderer, compare with the capture's reference
//
// Runs the capture's PM4 through the same executor the game uses in-process.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include <rex/ui/vulkan/provider.h>

#include "../image_diff.h"
#include "../pm4.h"
#include "../vk/renderer.h"
#include "capture_file.h"

namespace {

namespace rg = rex::graphics;
using bots::gfx::Draw;
using bots::gfx::ShaderLoad;
using bots::gfx::replay::Capture;

const char* EntryName(uint32_t entry) {
  switch (entry) {
    case 0x82335478: return "DrawIndexedVertices";
    case 0x82335088: return "DrawVertices";
    case 0x82334A70: return "DrawVerticesUP";
    case 0x82334FE8: return "DrawIndexedVerticesUP";
    case 0x82334A60: return "EndVertices";
    case 0x8231D5B8: return "Resolve";
    case 0x8231C9C0: return "Clear?";
    case 0x82317110: return "Swap";
    case 0x82333720: return "BeginTiling";
    case 0x82333AB8: return "EndTiling";
    default: return nullptr;
  }
}

float F(const rg::RegisterFile& regs, uint32_t index) {
  float f;
  std::memcpy(&f, &regs.values[index], 4);
  return f;
}

struct RtKey {
  uint32_t surface, color[4], depth, mode, mask;
  auto operator<=>(const RtKey&) const = default;
};

class Logger : public bots::gfx::Pm4Sink {
 public:
  Logger(const Capture& capture, bool verbose) : capture_(capture), verbose_(verbose) {}
  bots::gfx::Pm4Executor* executor = nullptr;

  void OnDraw(const rg::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps, const Draw& draw) override {
    const size_t offset = executor->packet_offset();
    const int call = capture_.CallAt(offset);
    const uint32_t entry = call >= 0 ? capture_.calls[size_t(call)].entry : 0;

    const auto surface = regs.Get<rg::reg::RB_SURFACE_INFO>();
    const auto depth = regs.Get<rg::reg::RB_DEPTH_INFO>();
    const auto mode = regs.Get<rg::reg::RB_MODECONTROL>();
    RtKey key{surface.value, {}, depth.value, mode.value, regs.values[rg::XE_GPU_REG_RB_COLOR_MASK]};
    for (int i = 0; i < 4; ++i) key.color[i] = regs.values[rg::reg::RB_COLOR_INFO::rt_register_indices[i]];
    ++render_targets_[key];
    vertex_shaders_.insert({vs.address, vs.dwords});
    pixel_shaders_.insert({ps.address, ps.dwords});
    ++prims_[uint32_t(draw.initiator.prim_type)];

    if (!verbose_) return;
    const auto color0 = regs.Get<rg::reg::RB_COLOR_INFO>();
    const char* name = EntryName(entry);
    std::printf("draw %4llu pm4+%06zx call %6d %-22s prim=%2u src=%u n=%5u idx%u base=%08X words=%u", ++draws_,
                offset, call, name ? name : "?", uint32_t(draw.initiator.prim_type),
                uint32_t(draw.initiator.source_select), uint32_t(draw.initiator.num_indices),
                draw.initiator.index_size == rg::xenos::IndexFormat::kInt32 ? 32 : 16, draw.index_base,
                draw.index_words);
    std::printf(" | mode=%u pitch=%u msaa=%u c0=fmt%u@%u z=fmt%u@%u mask=%04X", uint32_t(mode.edram_mode),
                uint32_t(surface.surface_pitch), uint32_t(surface.msaa_samples), uint32_t(color0.color_format),
                uint32_t(color0.color_base), uint32_t(depth.depth_format), uint32_t(depth.depth_base),
                regs.values[rg::XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF);
    std::printf(" | vs=%08X/%u ps=%08X/%u | vp s=(%.1f,%.1f) o=(%.1f,%.1f)\n", vs.address, vs.dwords, ps.address,
                ps.dwords, F(regs, rg::XE_GPU_REG_PA_CL_VPORT_XSCALE), F(regs, rg::XE_GPU_REG_PA_CL_VPORT_YSCALE),
                F(regs, rg::XE_GPU_REG_PA_CL_VPORT_XOFFSET), F(regs, rg::XE_GPU_REG_PA_CL_VPORT_YOFFSET));
  }

  void OnEvent(uint32_t opcode, const uint32_t*, uint32_t) override { ++events_[opcode]; }

  void Report() const {
    std::printf("render-target configurations (draws):\n");
    for (const auto& [k, n] : render_targets_) {
      std::printf("  %5u  surface=%08X color=%08X %08X %08X %08X depth=%08X mode=%u mask=%08X\n", n, k.surface,
                  k.color[0], k.color[1], k.color[2], k.color[3], k.depth, k.mode, k.mask);
    }
    std::printf("primitive types:");
    for (const auto& [p, n] : prims_) std::printf(" %u:%u", p, n);
    std::printf("\nshaders: %zu vertex, %zu pixel\nother type-3 packets:", vertex_shaders_.size(),
                pixel_shaders_.size());
    for (const auto& [op, n] : events_) std::printf(" %02X:%u", op, n);
    std::printf("\n");
  }

 private:
  const Capture& capture_;
  bool verbose_;
  unsigned long long draws_ = 0;
  std::map<RtKey, uint32_t> render_targets_;
  std::set<std::pair<uint32_t, uint32_t>> vertex_shaders_, pixel_shaders_;
  std::map<uint32_t, uint32_t> prims_, events_;
};

bool ReadPpm(const std::filesystem::path& path, std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  unsigned w, h, max;
  const bool ok = std::fscanf(f, "P6 %u %u %u", &w, &h, &max) == 3 && max == 255 && std::fgetc(f) != EOF;
  if (ok) {
    rgb.resize(size_t(w) * h * 3);
    width = w, height = h;
  }
  const bool read = ok && std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
  std::fclose(f);
  return read;
}

// Renders the capture with the native renderer, writes `out`, compares with the capture's reference.ppm.
int Render(Capture& capture, const std::filesystem::path& out, int repeat) {
  auto provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
  if (!provider) throw std::runtime_error("no Vulkan device");
  bots::gfx::vk::Renderer renderer(provider->vulkan_device(),
                                   [&](uint32_t pa) -> const uint8_t* { return capture.memory.Physical(pa); });
  if (!renderer.ok()) throw std::runtime_error("renderer setup failed");
  bots::gfx::Pm4Executor executor([&](uint32_t pa) -> const uint8_t* { return capture.memory.Physical(pa); },
                                  &renderer);
  // Repeats replay the frame again over the warm caches, as the next frames of a running game would.
  for (int i = 0; i < repeat; ++i) {
    if (i) {
      renderer.EndFrame();
      if (std::getenv("BOTS_TRACE_RENDERING")) std::fprintf(stderr, "Frame %d\n", i);
    }
    executor.Execute(capture.pm4.data(), capture.pm4.size());
  }
  renderer.Finish();
  const auto& s = renderer.stats();
  std::printf("native: %llu resolves (%llu dropped, %llu multisampled not copied), %llu clears, %llu draws, "
              "%llu draws not rendered yet\n",
              (unsigned long long)s.resolves, (unsigned long long)s.dropped_resolves,
              (unsigned long long)s.msaa_resolves, (unsigned long long)s.clears,
              (unsigned long long)s.draws, (unsigned long long)s.skipped_draws);
  std::printf("native: %llu textures loaded, %llu unsupported, %llu EDRAM transfers\n",
              (unsigned long long)s.textures, (unsigned long long)s.unsupported_textures,
              (unsigned long long)s.transfers);
  std::printf("native: %llu render passes, %llu pipelines, %llu failed shaders, %llu unsupported primitives\n",
              (unsigned long long)s.render_passes, (unsigned long long)s.pipelines,
              (unsigned long long)s.failed_shaders, (unsigned long long)s.unsupported_primitives);

  std::vector<uint8_t> image, reference;
  uint32_t w, h, rw, rh;
  if (!renderer.ReadTexture(renderer.last_resolve_address(), image, w, h)) {
    throw std::runtime_error("nothing resolved");
  }
  if (std::FILE* f = std::fopen((capture.dir / "gamma.bin").c_str(), "rb")) {
    uint32_t table[256];
    if (std::fread(table, 4, 256, f) == 256) {
      bots::gfx::ApplyGammaRamp(image, table);
      std::printf("gamma ramp applied\n");
    }
    std::fclose(f);
  }
  if (!bots::gfx::WritePpm(out, image.data(), w, h, size_t(w) * 3, 3)) {
    throw std::runtime_error("cannot write " + out.string());
  }
  std::printf("frame: %ux%u from resolve to %08X -> %s\n", w, h, renderer.last_resolve_address(), out.c_str());
  if (!ReadPpm(capture.dir / "reference.ppm", reference, rw, rh)) {
    std::printf("no reference.ppm\n");
    return 0;
  }
  // Compare the overlapping area (the reference is the presenter's output and may be a different size).
  const auto d = bots::gfx::CompareImages(image, w, h, reference.data(), rw, rh, size_t(rw) * 3, 3);
  std::printf("vs reference %ux%u: mean abs diff %.2f, PSNR %.1f dB, %.2f%% pixels off by >16\n", rw, rh,
              d.mean_abs, d.psnr, d.bad_percent);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: bots-replay CAPDIR [--draws] [--render OUT.ppm [--repeat N]]\n");
    return 2;
  }
  bool verbose = false;
  const char* render_out = nullptr;
  int repeat = 1;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--draws") == 0) verbose = true;
    if (std::strcmp(argv[i], "--render") == 0 && i + 1 < argc) render_out = argv[++i];
    if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) repeat = std::max(1, std::atoi(argv[++i]));
  }
  try {
    Capture capture;
    capture.Load(argv[1]);
    std::printf("frame %u: %zu calls, %zu KB PM4, device %08X, front buffer %08X\n", capture.frame,
                capture.calls.size(), capture.pm4.size() >> 10, capture.device, capture.front_buffer);
    if (render_out) return Render(capture, render_out, repeat);

    Logger logger(capture, verbose);
    bots::gfx::Pm4Executor executor([&](uint32_t pa) -> const uint8_t* { return capture.memory.Physical(pa); },
                                    &logger);
    logger.executor = &executor;
    executor.Execute(capture.pm4.data(), capture.pm4.size());

    const auto& s = executor.stats();
    std::printf("packets %llu, draws %llu, register writes %llu, other type-3 %llu, indirect skipped %llu\n",
                (unsigned long long)s.packets, (unsigned long long)s.draws, (unsigned long long)s.register_writes,
                (unsigned long long)s.unknown_packets, (unsigned long long)s.skipped_indirect);
    logger.Report();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bots-replay: %s\n", e.what());
    return 1;
  }
  return 0;
}
