// Plain-assert tests for src/fps_math.h. Run: c++ -std=c++20 -I src tests/fps_math_test.cpp -o /tmp/t && /tmp/t
#include <cmath>
#include <cstdio>

#include "fps_math.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(double a, double b, double e) { return std::fabs(a - b) <= e; }

int main() {
  bots::fps::Meter m(0.5);
  // The first call only starts a window.
  CHECK(!m.Update(1000, 10.0));
  CHECK(!m.has_value());
  // Inside the window nothing changes, however often it is asked.
  CHECK(!m.Update(1010, 10.2));
  CHECK(!m.Update(1020, 10.4));
  // 30 frames in 0.5 s: 60 fps, 16.67 ms.
  CHECK(m.Update(1030, 10.5));
  CHECK(Near(m.fps(), 60.0, 1e-9));
  CHECK(Near(m.frame_ms(), 1000.0 / 60.0, 1e-9));
  // The next window starts where the last ended: 15 frames in 0.5 s is 30 fps.
  CHECK(m.Update(1045, 11.0));
  CHECK(Near(m.fps(), 30.0, 1e-9));
  CHECK(Near(m.frame_ms(), 33.333333, 1e-4));
  // A late sample covers the time that really passed: 60 frames in 2 s.
  CHECK(m.Update(1105, 13.0));
  CHECK(Near(m.fps(), 30.0, 1e-9));
  // A stall: no frames for a whole window reads 0 fps.
  CHECK(m.Update(1105, 13.6));
  CHECK(m.fps() == 0.0 && m.frame_ms() == 0.0);
  // A counter that went backwards (reset) restarts the window instead of underflowing.
  CHECK(!m.Update(5, 14.0));
  CHECK(m.Update(35, 14.5));
  CHECK(Near(m.fps(), 60.0, 1e-9));

  std::printf(failures ? "%d failed\n" : "ok\n", failures);
  return failures ? 1 : 0;
}
