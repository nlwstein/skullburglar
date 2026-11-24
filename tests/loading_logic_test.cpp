// Plain-assert tests for src/setup/loading_logic.h (the loading screen's numbers).
// Run: c++ -std=c++20 -I src tests/loading_logic_test.cpp -o /tmp/t && /tmp/t
#include "setup/loading_logic.h"

#include <cassert>
#include <cstdio>

using namespace bots::setup;

// Two recorded starts (the game's "loading screen:" log lines, seconds since the screen appeared, shaders done / total):
// a cold first start (169 s) and one with the system's shader cache warm (12 s).
struct Pt { double t; uint32_t done, total; };
static const Pt kCold[] = {{0.5, 0, 2286}, {6.3, 414, 4653}, {11.8, 931, 4653}, {15.3, 1451, 4653}, {19.7, 1967, 4653},
                           {22.6, 2484, 4653}, {56.9, 2999, 4653}, {101.1, 3516, 4653}, {145.6, 4033, 4653},
                           {165.8, 4550, 4653}, {168.5, 4653, 4653}};
static const Pt kWarm[] = {{0.6, 0, 2286}, {2.7, 427, 4653}, {3.4, 933, 4653}, {3.9, 1456, 4653}, {4.5, 1966, 4653},
                           {5.1, 2489, 4653}, {5.6, 2999, 4653}, {7.2, 3519, 4653}, {8.6, 4037, 4653}, {11.3, 4557, 4653},
                           {12.1, 4653, 4653}};

// Replays a trace at 4 frames a second (counts interpolated between the log lines) and checks the rules that matter on screen.
// Returns the largest bucket level shown.
template <size_t N>
static int Replay(const Pt (&pts)[N], bool* hint_seen, int* max_level_after_half) {
  LoadingProgress p;
  p.Observe(0.0, false, 0, 0, false);
  int shown = -1, max_level = -1;
  double changed = 0;
  *hint_seen = false;
  *max_level_after_half = -1;
  for (double t = pts[0].t; t <= pts[N - 1].t; t += 0.25) {
    size_t i = 0;
    while (i + 2 < N && pts[i + 1].t < t) ++i;
    const double f = std::clamp((t - pts[i].t) / (pts[i + 1].t - pts[i].t), 0.0, 1.0);
    const uint32_t done = uint32_t(pts[i].done + f * (pts[i + 1].done - pts[i].done));
    p.Observe(t, true, done, pts[i + 1].total, done >= pts[i + 1].total);
    const std::string& text = p.remaining_text();
    if (text.empty()) continue;
    // Not before 3 s and 5 %.
    assert(t - pts[0].t >= 3.0 - 1e-9 && double(done) >= 0.05 * double(p.total()));
    int level = -1;
    for (int l = 0; l < 20; ++l) if (LevelText(l) == text) level = l;
    assert(level >= 0);
    // Never rises by more than one bucket within 10 s of the last change.
    if (shown >= 0 && level != shown) {
      assert(level <= shown + 1 || t - changed >= 10.0 - 1e-9);
      changed = t;
    } else if (shown < 0) {
      changed = t;
    }
    shown = level;
    max_level = std::max(max_level, level);
    // Past half way, never more than twice the time spent.
    if (double(done) > 0.5 * double(p.total())) {
      assert(p.remaining_seconds() <= 2.0 * (t - pts[0].t) + 1e-6);
      *max_level_after_half = std::max(*max_level_after_half, level);
    }
    if (p.show_first_start_hint()) *hint_seen = true;
  }
  return max_level;
}

int main() {
  // Before the graphics exist: starting up, bar at zero, nothing to estimate.
  {
    LoadingProgress p;
    p.Observe(0.0, false, 0, 0, false);
    assert(p.phase() == LoadPhase::kStarting);
    assert(p.steps().starting == StepState::kActive && p.steps().shaders == StepState::kPending);
    assert(p.bar() == 0.0f && !p.determinate() && p.remaining_seconds() < 0);
  }
  // A first start: 3000 pipelines at 12 a second. The bar never moves backward, the steps go in order, the estimate shows
  // up after a few seconds and only shrinks as the build goes on at a steady rate.
  {
    LoadingProgress p;
    p.Observe(0.0, false, 0, 0, false);
    float last_bar = 0.0f;
    double last_remaining = 1e9;
    bool saw_estimate = false, saw_hint = false;
    for (int t = 1; t <= 250; ++t) {
      const uint32_t done = uint32_t(std::min(3000.0, 12.0 * t));
      const bool fin = done >= 3000;
      p.Observe(double(t), true, done, 3000, fin);
      assert(p.bar() >= last_bar);
      last_bar = p.bar();
      if (!fin) {
        assert(p.phase() == LoadPhase::kShaders);
        assert(p.steps().shaders == StepState::kActive && p.steps().starting == StepState::kDone);
        assert(p.determinate() && p.building());
        const double r = p.remaining_seconds();
        if (t < 4) assert(r < 0);  // not before 3 s
        if (r >= 0) {
          saw_estimate = true;
          assert(r <= last_remaining + 2.0);  // steady rate: never jumps up
          last_remaining = r;
          const double truth = double(3000 - done) / 12.0;
          assert(r > truth * 0.85 - 1.0 && r < truth * 1.15 + 1.0);  // steady rate: the estimate is right
        }
        if (p.show_first_start_hint()) saw_hint = true;
      }
    }
    assert(saw_estimate && saw_hint);
    assert(p.phase() == LoadPhase::kGame && p.steps().game == StepState::kActive);
    assert(p.steps().shaders == StepState::kDone);
    assert(p.bar() >= 0.92f && p.bar() < 1.0f);
    // The game's own loading creeps toward the end but never claims to be done.
    for (int t = 251; t < 600; ++t) p.Observe(double(t), true, 3000, 3000, true);
    assert(p.bar() > 0.97f && p.bar() <= 0.99f);
  }
  // A warm start (the driver's cache): the count completes within seconds. No hint, no estimate, a moving stripe.
  {
    LoadingProgress p;
    p.Observe(0.0, false, 0, 0, false);
    p.Observe(1.0, true, 10, 120, false);
    p.Observe(2.0, true, 120, 120, true);
    assert(p.phase() == LoadPhase::kGame);
    assert(!p.show_first_start_hint() && p.remaining_seconds() < 0);
  }
  // Nothing queued at all: the game phase once the wait for queued pipelines is over; the bar still only moves forward.
  {
    LoadingProgress p;
    p.Observe(0.0, true, 0, 0, false);
    assert(p.phase() == LoadPhase::kStarting);
    p.Observe(20.5, true, 0, 0, true);
    assert(p.phase() == LoadPhase::kGame && !p.determinate());
  }
  // A count that goes backward or a smaller total (a reset) can't move the bar back or the numbers down.
  {
    LoadingProgress p;
    p.Observe(0.0, true, 500, 1000, false);
    const float b = p.bar();
    p.Observe(1.0, true, 100, 1000, false);
    assert(p.bar() >= b && p.done() >= 500);
    p.Observe(2.0, true, 600, 800, false);
    assert(p.total() >= 1000);
  }
  // The count growing after it completed (the new renderer's pipelines queue behind the port's): building again, bar kept.
  {
    LoadingProgress p;
    p.Observe(0.0, true, 0, 0, false);
    p.Observe(1.0, true, 1000, 2000, false);
    p.Observe(2.0, true, 2000, 2000, true);
    assert(p.phase() == LoadPhase::kGame);
    const float b = p.bar();
    p.Observe(3.0, true, 2100, 4000, false);
    assert(p.phase() == LoadPhase::kShaders && p.bar() >= b && p.total() == 4000);
    p.Observe(4.0, true, 4000, 4000, true);
    assert(p.phase() == LoadPhase::kGame);
  }
  // The two recorded starts.
  {
    bool hint = false;
    int after_half = 0;
    Replay(kCold, &hint, &after_half);
    assert(hint);  // a long first start explains itself
    bool warm_hint = true;
    const int warm_max = Replay(kWarm, &warm_hint, &after_half);
    assert(!warm_hint);                          // a fast start doesn't
    assert(warm_max <= 1 && after_half <= 1);    // and never claims minutes
  }
  // Wording.
  assert(RemainingText(-1).empty());
  assert(RemainingText(5) == "a few seconds left");
  assert(RemainingText(20) == "less than a minute left");
  assert(RemainingText(60) == "about 1 minute left");
  assert(RemainingText(89) == "about 1 minute left");
  assert(RemainingText(91) == "about 2 minutes left");
  assert(RemainingText(500) == "about 8 minutes left");
  assert(CountText(0) == "0" && CountText(812) == "812" && CountText(2686) == "2,686" && CountText(1234567) == "1,234,567");
  std::puts("loading_logic_test: ok");
  return 0;
}
