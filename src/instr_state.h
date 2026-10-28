// Test instrumentation (src/instr.cpp): counters the native renderer plugin (src/gfx/), which has no access to the
// game's symbols, writes for the game's status stream.
//
// The game owns the one instance and publishes its address in the process environment (BOTS_INSTR_STATE, hex) during
// static initialization, before any plugin loads (as prompt_state.h does), but only when BOTS_INSTR is set. Without
// it FindInstrState() is null and every renderer call site is one predictable branch on a cached pointer.
//
// Writers are single threads (the GPU command thread for frames and renderer counters); readers are the status
// thread. Everything is relaxed atomics: a status line may mix two frames' values, which is fine for a 2 Hz stream.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace bots {

struct InstrState {
  static constexpr uint32_t kVersion = 1;
  // Frame-time histogram: 0.5 ms buckets up to 128 ms, then one overflow bucket. Cumulative; readers take deltas.
  static constexpr uint32_t kBuckets = 257;
  static constexpr double kBucketMs = 0.5;

  uint32_t version = kVersion;

  // Guest swaps (what the game presents), GPU command thread. now_us: steady clock, microseconds.
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> last_frame_us{0};
  std::atomic<uint32_t> worst_frame_us{0};  // since the reader last reset it (exchange 0)
  std::atomic<uint32_t> hist[kBuckets] = {};

  // Port renderer (native) totals.
  std::atomic<uint64_t> draws{0}, presents{0}, pending_draws{0}, live_pipelines{0}, failed_pipeline_draws{0};
  std::atomic<uint32_t> render_w{0}, render_h{0};
  // The startup step ("Compiling shaders"): the port's pipelines and n1's step, done / total; hold = the guest is held;
  // loaded = the port's pipeline manifest is loaded, so warm_total is final (an empty list logs no "done" line).
  std::atomic<uint32_t> warm_done{0}, warm_total{0}, n1_warm_done{0}, n1_warm_total{0}, warm_hold{0}, warm_loaded{0};
  std::atomic<uint32_t> n1_warm_state{0};  // 0 not started, 1 running, 2 done (n1_precompile_progress)
  std::atomic<uint64_t> n1_warm_ms{0};     // the n1 step's duration once done

  // n1 (hybrid fill 4), per n1 frame: the last frame's numbers, and totals.
  std::atomic<uint64_t> n1_frames{0};
  std::atomic<uint32_t> n1_draws{0}, n1_instances{0}, n1_skipped{0}, n1_particles{0}, n1_meshes_pending{0},
      n1_loading{0};
  std::atomic<uint64_t> n1_particle_dropped{0};  // total sprites dropped by the caps
  // W2: pipelines/shaders built in play, late ones (asked for after the startup step), draws skipped / on the stand-in.
  std::atomic<uint32_t> n1_pipelines_live{0}, n1_shaders_live{0}, n1_late_pipelines{0}, n1_late_shaders{0},
      n1_build_failures{0}, n1_partial_frames{0};
  std::atomic<uint64_t> n1_draws_no_pipe{0}, n1_fallback_draws{0};
  std::atomic<uint32_t> n1_record_worst_us{0};  // since the reader reset it
  std::atomic<uint32_t> n1_failed{0};           // n1_create / n1_record failed: the hybrid fell back

  // Capture requests from the control channel (renderer side, present.cpp): a burst of `burst_count` consecutive
  // presented frames into burst_dir. The game bumps burst_seq after filling the fields; the renderer takes it.
  // burst_every_ms > 0 makes it a trace: every present is logged in the TSV, a frame is copied every that many ms (and
  // the first after any gap of 50 ms or more), up to burst_count copies.
  std::atomic<uint32_t> burst_seq{0}, burst_count{0}, burst_every_ms{0};
  char burst_dir[512] = {};
  // The status thread's current phase (instr.cpp ComputePhase), first 8 characters packed, for the burst TSV.
  std::atomic<uint64_t> phase8{0};
  std::atomic<uint32_t> bursts_written{0};

  static uint64_t NowUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
  }

  // GPU command thread, once per guest swap. ms: interval since the previous swap.
  void Frame(double ms) {
    frames.fetch_add(1, std::memory_order_relaxed);
    last_frame_us.store(NowUs(), std::memory_order_relaxed);
    uint32_t b = ms <= 0 ? 0 : uint32_t(ms / kBucketMs);
    if (b >= kBuckets) b = kBuckets - 1;
    hist[b].fetch_add(1, std::memory_order_relaxed);
    const uint32_t us = uint32_t(ms * 1000.0);
    if (us > worst_frame_us.load(std::memory_order_relaxed)) worst_frame_us.store(us, std::memory_order_relaxed);
  }
};

inline constexpr const char* kInstrStateEnv = "BOTS_INSTR_STATE";

// The game's instance, or null (instrumentation off, or another host such as bots-replay). Looked up once.
inline InstrState* FindInstrState() {
  static InstrState* state = [] {
    char value[32] = {};
#ifdef _WIN32
    if (!GetEnvironmentVariableA(kInstrStateEnv, value, sizeof(value))) return static_cast<InstrState*>(nullptr);
#else
    const char* v = std::getenv(kInstrStateEnv);
    if (!v) return static_cast<InstrState*>(nullptr);
    for (size_t i = 0; v[i] && i + 1 < sizeof(value); ++i) value[i] = v[i];
#endif
    auto* s = reinterpret_cast<InstrState*>(uintptr_t(std::strtoull(value, nullptr, 16)));
    return s && s->version == InstrState::kVersion ? s : nullptr;
  }();
  return state;
}

}  // namespace bots
