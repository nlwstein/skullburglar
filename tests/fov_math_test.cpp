// Plain-assert tests for src/fov_math.h. Run: c++ -std=c++20 -I src tests/fov_math_test.cpp -o /tmp/t && /tmp/t
#include <cmath>
#include <cstdio>

#include "fov_math.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(double a, double b, double e) { return std::fabs(a - b) <= e; }

int main() {
  using namespace bots::fov;
  // Offset 0 changes nothing, whatever the input.
  CHECK(Scale(0.0) == 1.0);
  CHECK(Apply(90.0, Scale(0.0)) == 90.0 && Apply(37.5, Scale(0.0)) == 37.5);
  // The original view plus the offset.
  CHECK(Near(Apply(kOriginal, Scale(15.0)), kOriginal + 15.0, 1e-9));
  CHECK(Near(Apply(kOriginal, Scale(40.0)), kOriginal + 40.0, 1e-9));
  // A narrower (zoomed) view widens too, but by less than the offset, and stays narrower than the normal one.
  const double zoom = Apply(40.0, Scale(30.0));
  CHECK(zoom > 40.0 && zoom < 40.0 + 30.0);
  CHECK(Near(std::tan(zoom * kPi / 360.0) / std::tan(40.0 * kPi / 360.0), Scale(30.0), 1e-9));
  CHECK(Apply(20.0, Scale(30.0)) < Apply(kOriginal, Scale(30.0)));
  // Bad input: clamped offsets, NaN, values outside (0, 180) come back as they are.
  CHECK(Scale(-10.0) == 1.0 && Scale(NAN) == 1.0);
  CHECK(Scale(500.0) == Scale(kMaxOffset));
  CHECK(Apply(0.0, 1.5) == 0.0 && Apply(-5.0, 1.5) == -5.0 && Apply(180.0, 1.5) == 180.0 && Apply(200.0, 1.5) == 200.0);
  CHECK(Apply(90.0, 0.0) == 90.0 && Apply(90.0, -1.0) == 90.0 && Apply(90.0, NAN) == 90.0);
  // The result never reaches 180.
  CHECK(Apply(170.0, Scale(40.0)) < 180.0);
  if (failures == 0) std::printf("fov_math_test: ok\n");
  return failures ? 1 : 0;
}
