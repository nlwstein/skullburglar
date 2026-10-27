// Plain-assert tests for src/input_math.h. Run: c++ -std=c++20 -I src tests/input_math_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>

#include "input_math.h"

using namespace bots::input;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(double a, double b, double e) { return std::fabs(a - b) <= e; }

int main() {
  // The original setting (20%) and anything above it pass the stick through untouched.
  CHECK((RemapRightStick({1234, -5678}, 0.2) == Stick{1234, -5678}));
  CHECK((RemapRightStick({-32768, 100}, 0.25) == Stick{-32768, 100}));

  // Dead zone 0: the game's own dead zone is cancelled, so the game's output equals the stick on every axis.
  for (int v : {1, 100, 1000, 6000, 12000, 20000, 32000}) {
    for (int sign : {1, -1}) {
      const Stick out = RemapRightStick({int16_t(sign * v), 0}, 0.0);
      CHECK(Near(GameAxisDeadZone(out.x), sign * v / 32768.0, 2.0 / 32768));
      CHECK(out.y == 0);
    }
  }
  // Diagonal keeps its direction (no per-axis snapping).
  {
    const Stick out = RemapRightStick({3000, 6000}, 0.0);
    const double gx = GameAxisDeadZone(out.x), gy = GameAxisDeadZone(out.y);
    CHECK(gx > 0 && gy > 0);
    CHECK(Near(gy / gx, 2.0, 0.01));
  }
  // A round dead zone: inside it nothing, just outside it a little, at full travel full.
  CHECK((RemapRightStick({0, 0}, 0.1) == Stick{}));
  CHECK((RemapRightStick({2000, 2000}, 0.1) == Stick{}));  // |v| = 0.086
  {
    const Stick out = RemapRightStick({5000, 0}, 0.1);  // 0.153 of travel
    CHECK(Near(GameAxisDeadZone(out.x), (0.15259 - 0.1) / 0.9, 0.002));
  }
  CHECK((RemapRightStick({32767, 0}, 0.1).x >= 32766));
  CHECK((RemapRightStick({-32768, 0}, 0.0).x <= -32766));
  // Magnitude stays monotonic with the dead zone: a smaller dead zone never gives less.
  CHECK(GameAxisDeadZone(RemapRightStick({9000, 0}, 0.0).x) > GameAxisDeadZone(RemapRightStick({9000, 0}, 0.15).x));

  // Response curve: squared yaw as shipped, linear with TurnPower 1 (table in the plan: 0.375 -> 14%).
  CHECK(Near(YawResponse(0.375, 2.0), 0.1406, 0.001));
  CHECK(Near(YawResponse(-0.375, 2.0), -0.1406, 0.001));
  CHECK(Near(YawResponse(0.375, 1.0), 0.375, 1e-9));
  CHECK(Near(YawResponse(1.0, 2.0), 1.0, 1e-9));

  // The LagAmount trap: 1 means the camera never turns; the shipped 1.1 is 0.147 at 60 fps; "no lag" is ~1.
  CHECK(LagProportion(1.0, 1.0 / 60, 1.0) == 0.0);
  CHECK(Near(LagProportion(1.1, 1.0 / 60, 1.0), 0.147, 0.001));
  CHECK(LagProportion(kNoLagAmount, 1.0 / 60, 1.0) > 0.999999);
  CHECK(LagProportion(kNoLagAmount, 1.0 / 240, 1.0) > 0.99);

  if (failures) return 1;
  std::puts("input_math: all passed");
  return 0;
}
