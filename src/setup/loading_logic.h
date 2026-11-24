// What the loading screen shows (loading_screen.h) worked out from what the game reports: a bar that only moves forward,
// a time-left estimate that doesn't jump around, and the wording of each step. Plain C++ so tests/loading_logic_test.cpp
// can run it alone.
//
// The game reports one measurable thing while it starts: how many of the stored shader pipelines are built (done /
// total). The first start builds all of them (minutes), later starts find them in the graphics driver's cache (seconds).
// Everything else (starting up, loading the game's front end) has no count, so the bar spends the last stretch creeping
// toward the end instead of pretending to know.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bots::setup {

enum class LoadPhase {
  kStarting,  // before the graphics are up
  kShaders,   // building the stored shader pipelines (done / total)
  kGame,      // shaders done (or none to build): the game loads its front end
};

// The three steps the screen lists, in order.
enum class StepState { kPending, kActive, kDone };

struct LoadingSteps {
  StepState starting = StepState::kActive;
  StepState shaders = StepState::kPending;
  StepState game = StepState::kPending;
};

class LoadingProgress {
 public:
  // Seconds since the screen first appeared; `done` / `total` the shader count (total 0: nothing counted yet or nothing
  // to build); `shaders_finished`: the count is complete (or nothing was queued for long enough); `graphics_up`: the
  // graphics system exists.
  void Observe(double now, bool graphics_up, uint32_t done, uint32_t total, bool shaders_finished) {
    if (!started_) started_ = true, t0_ = now;
    now_ = now;
    if (!graphics_up) return;
    graphics_ = true;
    total_ = std::max(total_, total);
    done = std::min(done, total);
    if (total > 0 && !shaders_finished && done < total) {
      saw_compile_ = true;
      if (compile_start_ < 0) compile_start_ = now;
    }
    if (shaders_finished && total > 0) done = total;
    done_ = std::max(done_, done);
    if (shaders_finished) {
      if (!shaders_done_) shaders_done_at_ = now, shaders_done_ = true;
    } else if (shaders_done_ && total > 0 && total > finished_total_) {
      // The count grew after it completed (the port's pipelines done, the new renderer's queued behind them): back to
      // building. The bar stays where it was.
      shaders_done_ = false;
    }
    if (shaders_done_) finished_total_ = total_;
    // Rate: a smoothed rate over a window, so one slow or fast pipeline doesn't swing the estimate.
    if (compile_start_ >= 0 && !shaders_done_) {
      if (last_sample_t_ < 0) {
        last_sample_t_ = now, last_sample_done_ = done_;
      } else if (now - last_sample_t_ >= 1.0) {
        const double inst = double(done_ - last_sample_done_) / (now - last_sample_t_);
        rate_ = rate_ < 0 ? inst : rate_ * 0.85 + inst * 0.15;
        last_sample_t_ = now, last_sample_done_ = done_;
      }
    }
    UpdateBar();
  }

  LoadPhase phase() const {
    if (!graphics_) return LoadPhase::kStarting;
    // Counting has begun and isn't finished: shaders. Before the first count arrives the game is still starting.
    if (!shaders_done_) return total_ > 0 ? LoadPhase::kShaders : LoadPhase::kStarting;
    return LoadPhase::kGame;
  }

  LoadingSteps steps() const {
    LoadingSteps s;
    switch (phase()) {
      case LoadPhase::kStarting: break;
      case LoadPhase::kShaders: s.starting = StepState::kDone, s.shaders = StepState::kActive; break;
      case LoadPhase::kGame:
        s.starting = s.shaders = StepState::kDone, s.game = StepState::kActive;
        break;
    }
    return s;
  }

  // True once a real shader build has been seen (not just a warm cache's few seconds): the screen then says "first
  // start" things and shows counts.
  bool building() const { return saw_compile_ && phase() == LoadPhase::kShaders; }
  bool saw_compile() const { return saw_compile_; }

  // The whole bar, 0..1, never moving backward. Without a shader build to count it isn't meaningful: see determinate().
  float bar() const { return bar_; }
  // False: nothing countable (a warm start): the screen shows a moving stripe instead of a fraction.
  bool determinate() const { return saw_compile_ && !(shaders_done_ && bar_ >= 0.995f); }

  uint32_t done() const { return done_; }
  uint32_t total() const { return total_; }

  // Seconds left in the shader build, or -1 while it can't be said (too early, no rate, not building).
  double remaining_seconds() const {
    if (phase() != LoadPhase::kShaders || compile_start_ < 0 || total_ == 0) return -1;
    const double since = now_ - compile_start_;
    if (since < 6.0 || done_ < 8 || double(done_) < 0.02 * double(total_)) return -1;
    const double rate = rate_ > 0 ? rate_ : double(done_) / since;
    if (!(rate > 0)) return -1;
    double left = double(total_ - done_) / rate;
    // Pipelines vary a lot in cost (a run of cheap ones can be followed by slow ones), so a short estimate early on
    // is a promise the build breaks: below 90 % it never says less than a minute.
    if (double(done_) < 0.9 * double(total_)) left = std::max(left, 60.0);
    return left;
  }

  // The hint about the first start shows once the build is clearly a long one.
  bool show_first_start_hint() const { return saw_compile_ && compile_start_ >= 0 && now_ - compile_start_ >= 4.0; }

  double seconds_since_shaders_done() const { return shaders_done_ ? now_ - shaders_done_at_ : 0.0; }

 private:
  // Shaders take the bar to 92 %; the game's own loading creeps from there toward 99 %, so the bar never claims to be done
  // before the game is.
  void UpdateBar() {
    float v = 0.0f;
    if (graphics_) v = 0.02f;
    if (total_ > 0) v = std::max(v, 0.02f + 0.90f * float(done_) / float(total_));
    if (shaders_done_) {
      const double t = now_ - shaders_done_at_;
      v = std::max(v, 0.92f + 0.07f * float(1.0 - std::exp(-t / 8.0)));
    }
    bar_ = std::max(bar_, std::min(v, 0.99f));
  }

  bool started_ = false, graphics_ = false, saw_compile_ = false, shaders_done_ = false;
  double t0_ = 0, now_ = 0, compile_start_ = -1, last_sample_t_ = -1, rate_ = -1;
  double shaders_done_at_ = -1;
  uint32_t done_ = 0, total_ = 0, last_sample_done_ = 0, finished_total_ = 0;
  float bar_ = 0.0f;
};

// "about 2 min left" / "less than a minute left": coarse on purpose (an exact count of seconds only jitters).
inline std::string RemainingText(double seconds) {
  if (!(seconds >= 0)) return "";
  if (seconds < 45) return "less than a minute left";
  const int minutes = int(std::ceil(seconds / 60.0));
  char buf[48];
  std::snprintf(buf, sizeof(buf), minutes == 1 ? "about 1 minute left" : "about %d minutes left", minutes);
  return buf;
}

// "2,686" (thousands separators: the counts read at a glance).
inline std::string CountText(uint32_t n) {
  std::string digits = std::to_string(n), out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

}  // namespace bots::setup
