// bots-replay: renders a D3D frame capture offline (native renderer, P2).
//
//   bots-replay CAPDIR [--draws]
//
// Runs the capture's PM4 through the same executor the game uses in-process and reports the state of every draw.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include "../pm4.h"
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: bots-replay CAPDIR [--draws]\n");
    return 2;
  }
  const bool verbose = argc > 2 && std::strcmp(argv[2], "--draws") == 0;
  try {
    Capture capture;
    capture.Load(argv[1]);
    std::printf("frame %u: %zu calls, %zu KB PM4, device %08X, front buffer %08X\n", capture.frame,
                capture.calls.size(), capture.pm4.size() >> 10, capture.device, capture.front_buffer);

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
