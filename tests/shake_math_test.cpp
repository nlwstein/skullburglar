// Plain-assert tests for src/shake_math.h. Run: c++ -std=c++20 -I src tests/shake_math_test.cpp -o /tmp/t && /tmp/t
#include <climits>
#include <cmath>
#include <cstdio>

#include "shake_math.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(double a, double b, double e) { return std::fabs(a - b) <= e; }

int main() {
  using namespace bots::shake;
  // The percentage becomes a factor in 0..1.
  CHECK(Factor(100.0) == 1.0f && Factor(0.0) == 0.0f && Near(Factor(25.0), 0.25, 1e-6));
  CHECK(Factor(250.0) == 1.0f && Factor(-5.0) == 0.0f && Factor(NAN) == 1.0f);
  // Floats: factor 1 returns the game's value untouched, 0 returns the input, between scales the offset.
  CHECK(Scaled(10.0f, 14.0f, 1.0f) == 14.0f);
  CHECK(Scaled(10.0f, 14.0f, 0.0f) == 10.0f);
  CHECK(Near(Scaled(10.0f, 14.0f, 0.25f), 11.0, 1e-6));
  CHECK(Near(Scaled(10.0f, 6.0f, 0.5f), 8.0, 1e-6));
  // Rotation ints: same rules, the sign of the offset is kept.
  CHECK(Scaled(1000, 1400, 1.0f) == 1400);
  CHECK(Scaled(1000, 1400, 0.0f) == 1000);
  CHECK(Scaled(1000, 1400, 0.25f) == 1100);
  CHECK(Scaled(1000, 600, 0.5f) == 800);
  // The offset is computed with wraparound: a shake across the int boundary is still a small offset.
  CHECK(Scaled(INT_MAX - 10, INT_MIN + 9, 0.5f) == INT_MAX);
  CHECK(Scaled(INT_MIN + 10, INT_MAX - 9, 0.0f) == INT_MIN + 10);
  CHECK(Scaled(INT_MIN + 10, INT_MAX - 9, 0.5f) == INT_MIN);
  std::printf(failures ? "%d failed\n" : "ok\n", failures);
  return failures ? 1 : 0;
}
