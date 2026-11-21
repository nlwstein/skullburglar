// Plain-assert tests for src/aim_toggle.h. Run: c++ -std=c++20 -I src tests/aim_toggle_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "aim_toggle.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using bots::aim::Toggle;
using bots::aim::Update;

int main() {
  // Off: the trigger passes through untouched (a hold), and nothing latches.
  {
    Toggle t;
    CHECK(Update(t, false, true, 0, false) == 0);
    CHECK(Update(t, false, true, 255, false) == 255);
    CHECK(Update(t, false, true, 130, false) == 130);
    CHECK(Update(t, false, true, 0, false) == 0 && !t.latched);
  }
  // On: press latches, release keeps it, the next press unlatches, release keeps it off.
  {
    Toggle t;
    CHECK(Update(t, true, true, 0, false) == 0);
    CHECK(Update(t, true, true, 255, false) == 255);  // press: aiming on
    CHECK(Update(t, true, true, 255, false) == 255);  // still held
    CHECK(Update(t, true, true, 0, false) == 255);    // released: stays on
    CHECK(Update(t, true, true, 0, false) == 255);
    CHECK(Update(t, true, true, 255, false) == 0);    // second press: off, though still held
    CHECK(Update(t, true, true, 255, false) == 0);
    CHECK(Update(t, true, true, 0, false) == 0);      // released: stays off
  }
  // A light pull below the threshold is not a press; a pad's analog pull above it is.
  {
    Toggle t;
    CHECK(Update(t, true, true, 40, false) == 0);
    CHECK(Update(t, true, true, 200, false) == 255);
    CHECK(Update(t, true, true, 0, false) == 255);
  }
  // Two keys bound to Aim (RMB + Shift): the second key adds no edge, so it neither toggles twice nor cancels.
  {
    Toggle t;
    CHECK(Update(t, true, true, 255, false) == 255);  // Shift down
    CHECK(Update(t, true, true, 255, false) == 255);  // RMB down too: the merged trigger stays pressed
    CHECK(Update(t, true, true, 255, false) == 255);  // Shift up, RMB still down
    CHECK(Update(t, true, true, 0, false) == 255);    // RMB up: latched
  }
  // A (cover, run) ends a latched aim, once per press; a held A does not stop the next aim.
  {
    Toggle t;
    CHECK(Update(t, true, true, 255, false) == 255);
    CHECK(Update(t, true, true, 0, false) == 255);
    CHECK(Update(t, true, true, 0, true) == 0);       // A pressed: aim off
    CHECK(Update(t, true, true, 0, true) == 0);       // A held
    CHECK(Update(t, true, true, 255, true) == 255);   // aim again while A is held
    CHECK(Update(t, true, true, 0, true) == 255);
  }
  // A and Aim pressed together: aim wins (it latches on, or off if it was on).
  {
    Toggle t;
    CHECK(Update(t, true, true, 255, true) == 255);
  }
  // Not allowed (menu, pause, loading, cutscene, vehicle): pass-through, and the latch is dropped.
  {
    Toggle t;
    CHECK(Update(t, true, true, 255, false) == 255);
    CHECK(Update(t, true, true, 0, false) == 255);
    CHECK(Update(t, true, false, 0, false) == 0 && !t.latched);
    CHECK(Update(t, true, true, 0, false) == 0);       // back in play: not aiming
    CHECK(Update(t, true, false, 255, false) == 255);  // brake while driving: raw value
    CHECK(Update(t, true, true, 255, false) == 0);     // trigger still held as play resumes: no new press, not aiming
    CHECK(Update(t, true, true, 0, false) == 0);
    CHECK(Update(t, true, true, 255, false) == 255);   // a fresh press latches
  }
  // Turning the setting off drops a latch.
  {
    Toggle t;
    CHECK(Update(t, true, true, 255, false) == 255);
    CHECK(Update(t, false, true, 0, false) == 0 && !t.latched);
  }
  if (failures == 0) std::printf("aim_toggle_test: ok\n");
  return failures ? 1 : 0;
}
