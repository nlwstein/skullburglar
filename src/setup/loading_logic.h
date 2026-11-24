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
#include <utility>
#include <vector>

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

// The estimate's buckets: 0 a few seconds (under 15), 1 under a minute, then 1 + N for "about N minutes" (N rounded to the nearest
// minute, at least 1).
inline int EstimateLevel(double seconds) {
  if (seconds < 15) return 0;
  if (seconds < 60) return 1;
  return 1 + std::max(1, int(std::lround(seconds / 60.0)));
}
inline std::string LevelText(int level) {
  if (level < 0) return "";
  if (level == 0) return "a few seconds left";
  if (level == 1) return "less than a minute left";
  char buf[48];
  std::snprintf(buf, sizeof(buf), level == 2 ? "about 1 minute left" : "about %d minutes left", level - 1);
  return buf;
}

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
    // Rate: completed pipelines over the last few seconds (both queues count, the total grows when the second is queued).
    if (compile_start_ >= 0 && !shaders_done_) {
      samples_.emplace_back(now, done_);
      while (samples_.size() > 2 && samples_[1].first <= now - kRateWindow) samples_.erase(samples_.begin());
      const auto& s0 = samples_.front();
      if (now - s0.first >= 1.0) rate_ = double(done_ - s0.second) / (now - s0.first);
      UpdateEstimate();
    }
    if (phase() != LoadPhase::kShaders) text_.clear(), shown_seconds_ = -1;
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

  // Seconds left in the shader build as shown (see UpdateEstimate), or -1 while it can't be said.
  double remaining_seconds() const { return shown_seconds_; }
  // The text under the bar: held so it never jumps up by more than one step within 10 s; "" while there is no estimate.
  const std::string& remaining_text() const { return text_; }

  // The first-start hint shows (and stays) once the build is clearly a long one: many pipelines and a raw estimate of 20 s or more.
  bool show_first_start_hint() const { return hint_; }

  double seconds_since_shaders_done() const { return shaders_done_ ? now_ - shaders_done_at_ : 0.0; }

 private:
  // Shaders take the bar to 92 %; the game's own loading creeps from there toward 99 %, so the bar never claims to be done
  // before the game is.
  // remaining / rate, from 3 s and 5 % on; never longer than twice the time spent once past half way; the text is
  // bucketed (a few seconds, under a minute, about N minutes) and held if it would rise by more than one bucket within 10 s.
  void UpdateEstimate() {
    shown_seconds_ = -1;
    if (phase() != LoadPhase::kShaders || total_ == 0 || !(rate_ > 0)) return;
    const double since = now_ - compile_start_;
    if (since < 3.0) return;
    double est = double(total_ - done_) / rate_;
    if (total_ > 1000 && est >= 20.0) hint_ = true;
    const double frac = double(done_) / double(total_);
    if (frac < 0.05) return;
    if (frac > 0.5) est = std::min(est, 2.0 * since);
    shown_seconds_ = est;
    const int level = EstimateLevel(est);
    if (shown_level_ < 0 || level <= shown_level_ + 1 || now_ - level_changed_ >= 10.0) {
      if (level != shown_level_) level_changed_ = now_;
      shown_level_ = level;
    }
    text_ = LevelText(shown_level_);
  }

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
  double t0_ = 0, now_ = 0, compile_start_ = -1, rate_ = -1;
  static constexpr double kRateWindow = 5.0;
  std::vector<std::pair<double, uint32_t>> samples_;
  double shaders_done_at_ = -1, shown_seconds_ = -1, level_changed_ = 0;
  int shown_level_ = -1;
  bool hint_ = false;
  std::string text_;
  uint32_t done_ = 0, total_ = 0, finished_total_ = 0;
  float bar_ = 0.0f;
};

// The bucket text for an estimate in seconds (the screen shows LoadingProgress::remaining_text(), which also holds).
inline std::string RemainingText(double seconds) { return seconds >= 0 ? LevelText(EstimateLevel(seconds)) : ""; }

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
