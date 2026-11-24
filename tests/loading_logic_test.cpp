// Plain-assert tests for src/setup/loading_logic.h (the loading screen's numbers).
// Run: c++ -std=c++20 -I src tests/loading_logic_test.cpp -o /tmp/t && /tmp/t
#include "setup/loading_logic.h"

#include <cassert>
#include <cstdio>

using namespace bots::setup;

// Recorded starts (the game's "loading screen:" log lines, seconds since the screen appeared, shaders done / total):
// a cold first start (169 s), one with the system's shader cache warm (12 s), and rc116's cold start on macOS (216 s; the
// anchors are its screenshots: 50 % at 32 s with 2,484 done, 60 % at 76 s with 2,999, 90 % at 188 s with 4,554; the first
// half of the pipelines came from the cache in seconds, the rest compiled for minutes).
struct Pt { double t; uint32_t done, total; };
static const Pt kCold[] = {{0.5, 0, 2286}, {6.3, 414, 4653}, {11.8, 931, 4653}, {15.3, 1451, 4653}, {19.7, 1967, 4653},
                           {22.6, 2484, 4653}, {56.9, 2999, 4653}, {101.1, 3516, 4653}, {145.6, 4033, 4653},
                           {165.8, 4550, 4653}, {168.5, 4653, 4653}};
static const Pt kWarm[] = {{0.6, 0, 2286}, {2.7, 427, 4653}, {3.4, 933, 4653}, {3.9, 1456, 4653}, {4.5, 1966, 4653},
                           {5.1, 2489, 4653}, {5.6, 2999, 4653}, {7.2, 3519, 4653}, {8.6, 4037, 4653}, {11.3, 4557, 4653},
                           {12.1, 4653, 4653}};
static const Pt kRc116Cold[] = {{0.5, 0, 2286}, {6.0, 600, 4653}, {14.0, 1400, 4653}, {24.0, 2150, 4653}, {32.0, 2484, 4653},
                                {76.0, 2999, 4653}, {130.0, 3800, 4653}, {188.0, 4554, 4653}, {191.0, 4653, 4653}};

// Replays a trace at 4 frames a second (counts interpolated between the log lines). Returns when the first-start message
// first showed (-1: never) and checks the bar only moves forward and the message never goes away once shown.
template <size_t N>
static double Replay(const Pt (&pts)[N]) {
  LoadingProgress p;
  p.Observe(0.0, false, 0, 0, false);
  double hint_at = -1;
  float last_bar = 0;
  for (double t = pts[0].t; t <= pts[N - 1].t; t += 0.25) {
    size_t i = 0;
    while (i + 2 < N && pts[i + 1].t < t) ++i;
    const double f = std::clamp((t - pts[i].t) / (pts[i + 1].t - pts[i].t), 0.0, 1.0);
    const uint32_t done = uint32_t(pts[i].done + f * (pts[i + 1].done - pts[i].done));
    p.Observe(t, true, done, pts[i + 1].total, done >= pts[i + 1].total);
    assert(p.bar() >= last_bar);
    last_bar = p.bar();
    if (p.show_first_start_hint() && hint_at < 0) hint_at = t;
    if (hint_at >= 0 && p.phase() == LoadPhase::kShaders) assert(p.show_first_start_hint());  // stable once shown
  }
  return hint_at;
}

int main() {
  // Before the graphics exist: starting up, bar at zero, nothing to estimate.
  {
    LoadingProgress p;
    p.Observe(0.0, false, 0, 0, false);
    assert(p.phase() == LoadPhase::kStarting);
    assert(p.steps().starting == StepState::kActive && p.steps().shaders == StepState::kPending);
    assert(p.bar() == 0.0f && !p.determinate() && !p.show_first_start_hint());
  }
  // A first start: 3000 pipelines at 12 a second. The bar never moves backward, the steps go in order, and the first-start
  // message shows from 8 s and stays.
  {
    LoadingProgress p;
    p.Observe(0.0, false, 0, 0, false);
    float last_bar = 0.0f;
    bool saw_hint = false;
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
        if (t < 8) assert(!p.show_first_start_hint());
        if (p.show_first_start_hint()) saw_hint = true;
        if (saw_hint) assert(p.show_first_start_hint());
      }
    }
    assert(saw_hint);
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
    assert(!p.show_first_start_hint());
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
  // The recorded starts: the message explains the long ones from 8 s and a fast start never shows it.
  {
    assert(Replay(kCold) >= 8.0 && Replay(kCold) < 9.0);
    assert(Replay(kRc116Cold) >= 8.0 && Replay(kRc116Cold) < 9.0);  // rc116: was "a few seconds left" at 50 %, 160 s early
    assert(Replay(kWarm) < 0);
  }
  // Wording: no time estimate anywhere (nothing can say "a few seconds" while minutes remain), the message is a constant.
  {
    const std::string hint = kFirstStartHint;
    assert(hint.find("second") == std::string::npos && hint.find(" left") == std::string::npos);
    assert(hint == "The first start prepares shaders for your graphics card; this can take a few minutes.");
  }
  assert(CountText(0) == "0" && CountText(812) == "812" && CountText(2686) == "2,686" && CountText(1234567) == "1,234,567");
  std::puts("loading_logic_test: ok");
  return 0;
}
