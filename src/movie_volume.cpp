// Movie volume (GitHub #48): a launcher setting for how loud the pre-rendered Bink movies are.
//
// While a full-screen movie plays (the story cutscenes, music videos, the title attract movie), the engine's frame
// pacing function UGameEngine::GetMaxTickRate (sub_827CF730) sees a non-zero word at 0x8381A9F0 (docs/knowledge/
// frame-timing.md). Wrapping it keeps a host flag in step with that word, once per engine tick.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <cstring>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "movie_volume_math.h"

REXCVAR_DEFINE_DOUBLE(movie_volume, 100.0, "bots/Audio",
                      "Volume of the pre-rendered movies (cutscenes, music videos) in percent of their own level: "
                      "100 is as the game plays them, 200 is twice as loud, 0 is silent")
    .range(0.0, 200.0);

extern "C" void __imp__sub_827CF730(PPCContext& __restrict ctx, uint8_t* base);

namespace bots::movie {
std::atomic<bool> g_in_movie{false};
std::atomic<float> g_target{1.0f};  // the gain for the current movie state, refreshed every engine tick
// BOTS_MOVIE_TRACE=1: the loudest sample before and after the filter since the last log line (audio thread writes,
// the game thread reads and resets), so the setting can be measured without a speaker.
std::atomic<float> g_peak_in{0.0f}, g_peak_out{0.0f};
std::atomic<bool> g_trace{false};

// Runs on the audio thread for every 256-sample frame handed to the device. Only the pre-rendered movies are touched:
// outside one the target is 1 and the block is left alone.
void OutputFilter(float* interleaved, size_t samples, uint32_t channels) {
  static float applied = 1.0f;  // the gain the previous block ended on (the audio thread is the only user)
  const float target = g_target.load(std::memory_order_relaxed);
  const bool trace = g_trace.load(std::memory_order_relaxed);
  if (target == 1.0f && applied == 1.0f && !trace) return;
  float in = 0.0f;
  if (trace) {
    for (size_t i = 0; i < samples; ++i) in = std::max(in, std::fabs(interleaved[i]));
  }
  ApplyRamp(interleaved, samples, channels, applied, target);
  applied = target;
  if (trace) {
    float out = 0.0f;
    for (size_t i = 0; i < samples; ++i) out = std::max(out, std::fabs(interleaved[i]));
    if (in > g_peak_in.load(std::memory_order_relaxed)) g_peak_in.store(in, std::memory_order_relaxed);
    if (out > g_peak_out.load(std::memory_order_relaxed)) g_peak_out.store(out, std::memory_order_relaxed);
  }
}
}  // namespace bots::movie

extern "C" void sub_827CF730(PPCContext& __restrict ctx, uint8_t* base) {
  constexpr uint32_t kMovieWord = 0x8381A9F0;  // below 0xE0000000: no offset in the host mapping
  static const bool installed = [] {
    rex::audio::SetOutputFilter(&bots::movie::OutputFilter);
    return true;
  }();
  (void)installed;
  static const bool trace = [] {
    const char* v = std::getenv("BOTS_MOVIE_TRACE");
    const bool on = v && *v == '1';
    bots::movie::g_trace.store(on);
    return on;
  }();
  static auto next_log = std::chrono::steady_clock::now();
  uint32_t word;
  std::memcpy(&word, base + kMovieWord, 4);
  const bool playing = word != 0;  // big-endian; any non-zero value reads non-zero
  const float gain = playing ? bots::movie::Factor(REXCVAR_GET(movie_volume)) : 1.0f;
  bots::movie::g_target.store(gain, std::memory_order_relaxed);
  if (bots::movie::g_in_movie.exchange(playing) != playing) {
    REXLOG_INFO("movie: full-screen movie {} (movie volume {:.0f}%)", playing ? "started" : "ended",
                REXCVAR_GET(movie_volume));
  }
  if (trace && playing && std::chrono::steady_clock::now() >= next_log) {
    next_log = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto db = [](float v) { return v > 0.0f ? 20.0f * std::log10(v) : -120.0f; };
    const float in = bots::movie::g_peak_in.exchange(0.0f), out = bots::movie::g_peak_out.exchange(0.0f);
    REXLOG_INFO("movie trace: gain {:.2f}, device-mix peak {:.1f} dBFS before, {:.1f} dBFS after", gain, db(in), db(out));
  }
  __imp__sub_827CF730(ctx, base);
}
