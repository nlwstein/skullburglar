// Plain-assert tests for src/movie_volume_math.h. Run: c++ -std=c++20 -I src tests/movie_volume_math_test.cpp -o /tmp/t && /tmp/t
#include <cmath>
#include <cstdio>

#include "movie_volume_math.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(double a, double b, double e) { return std::fabs(a - b) <= e; }

int main() {
  using namespace bots::movie;
  // The percentage becomes a linear gain in 0..2.
  CHECK(Factor(100.0) == 1.0f && Factor(0.0) == 0.0f && Near(Factor(150.0), 1.5, 1e-6) && Near(Factor(25.0), 0.25, 1e-6));
  CHECK(Factor(200.0) == 2.0f && Factor(900.0) == 2.0f && Factor(-5.0) == 0.0f && Factor(NAN) == 1.0f);
  // Below the knee the limiter is transparent; above it the output stays under full scale and keeps its sign.
  CHECK(SoftLimit(0.5f) == 0.5f && SoftLimit(-0.8f) == -0.8f && SoftLimit(0.0f) == 0.0f);
  CHECK(SoftLimit(1.0f) > 0.8f && SoftLimit(1.0f) < 1.0f);
  CHECK(SoftLimit(-4.0f) < 0.0f && SoftLimit(-4.0f) >= -1.0f && SoftLimit(40.0f) <= 1.0f);
  CHECK(SoftLimit(0.9f) < SoftLimit(1.2f) && SoftLimit(1.2f) < SoftLimit(2.0f));  // monotonic
  CHECK(std::isnan(SoftLimit(NAN)));
  // Apply: unity is untouched, a cut is a plain scale, a boost of a quiet signal is a plain scale too.
  float a[4] = {0.5f, -0.25f, 1.0f, 0.0f};
  Apply(a, 4, 1.0f);
  CHECK(a[0] == 0.5f && a[1] == -0.25f && a[2] == 1.0f);
  Apply(a, 4, 0.5f);
  CHECK(a[0] == 0.25f && a[1] == -0.125f && a[2] == 0.5f && a[3] == 0.0f);
  float b[3] = {0.1f, -0.2f, 0.9f};
  Apply(b, 3, 2.0f);
  CHECK(Near(b[0], 0.2, 1e-6) && Near(b[1], -0.4, 1e-6));  // 0.2 and 0.4 are under the knee
  CHECK(b[2] > 0.8f && b[2] < 1.0f);                       // 1.8 is limited, not clipped flat
  float c[2] = {1.0f, -1.0f};
  Apply(c, 2, 2.0f);
  CHECK(c[0] <= 1.0f && c[1] >= -1.0f && c[0] > 0.8f && Near(c[0], -c[1], 1e-6));
  // Zero mutes.
  float d[2] = {0.7f, -0.7f};
  Apply(d, 2, Factor(0.0));
  CHECK(d[0] == 0.0f && d[1] == 0.0f);
  // Ramp: the last frame reaches the target, the first is one step along, equal gains match Apply.
  float r[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};  // 4 frames x 2 channels
  ApplyRamp(r, 8, 2, 1.0f, 0.0f);
  CHECK(Near(r[0], 0.5 * 0.75, 1e-6) && Near(r[2], 0.25, 1e-6) && Near(r[4], 0.125, 1e-6) && r[6] == 0.0f && r[7] == 0.0f);
  float e[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  ApplyRamp(e, 4, 2, 1.5f, 1.5f);
  CHECK(Near(e[0], 0.15, 1e-6) && Near(e[3], 0.6, 1e-6));
  float z[2] = {0.9f, -0.9f};
  ApplyRamp(z, 2, 2, 1.0f, 2.0f);  // one frame: goes straight to the target and the limiter
  CHECK(z[0] <= 1.0f && z[0] > 0.8f && z[1] >= -1.0f);
  ApplyRamp(z, 2, 0, 1.0f, 2.0f);  // zero channels: harmless
  if (failures == 0) std::printf("movie_volume_math ok\n");
  return failures ? 1 : 0;
}
