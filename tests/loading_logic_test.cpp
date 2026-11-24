// Plain-assert tests for src/setup/loading_logic.h (the loading screen's numbers).
// Run: c++ -std=c++20 -I src tests/loading_logic_test.cpp -o /tmp/t && /tmp/t
#include "setup/loading_logic.h"

#include <cassert>
#include <cstdio>

using namespace bots::setup;

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
        if (t < 6) assert(r < 0);
        if (r >= 0) {
          saw_estimate = true;
          assert(r <= last_remaining + 2.0);  // steady rate: never jumps up
          last_remaining = r;
          const double truth = double(3000 - done) / 12.0;
          assert(r > truth * 0.8 - 2.0 && r < truth * 1.2 + 2.0);
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
  // Wording.
  assert(RemainingText(-1).empty());
  assert(RemainingText(20) == "less than a minute left");
  assert(RemainingText(60) == "about 1 minute left");
  assert(RemainingText(61) == "about 2 minutes left");
  assert(RemainingText(500) == "about 9 minutes left");
  assert(CountText(0) == "0" && CountText(812) == "812" && CountText(2686) == "2,686" && CountText(1234567) == "1,234,567");
  std::puts("loading_logic_test: ok");
  return 0;
}
