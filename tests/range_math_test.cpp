// Plain-assert tests for src/range_math.h. Run: c++ -std=c++20 -I src tests/range_math_test.cpp -o /tmp/t && /tmp/t
#include <cmath>
#include <cstdio>
#include <string>

#include "range_math.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(double a, double b, double e = 0.01) { return std::fabs(a - b) <= e; }

int main() {
  using namespace bots::range;
  const Spot o{100, 200, 50};
  // Facing +x (yaw 0): targets straight ahead, spread along y and centred.
  const Spot a = TargetPos(o, 0, 1000, 150, 0, 3), b = TargetPos(o, 0, 1000, 150, 1, 3), c = TargetPos(o, 0, 1000, 150, 2, 3);
  CHECK(Near(a.x, 1100) && Near(a.y, 200 - 150) && Near(b.y, 200) && Near(c.y, 200 + 150) && Near(b.z, 50));
  // Facing +y (yaw 16384 = quarter turn): the line of fire turns with it.
  const Spot d = TargetPos(o, 16384, 1000, 150, 1, 3);
  CHECK(Near(d.x, 100, 0.1) && Near(d.y, 1200, 0.1));
  // A single target sits on the line of fire.
  CHECK(Near(TargetPos(o, 0, 500, 150, 0, 1).y, 200));
  // Yaw wraps like the engine's.
  CHECK(Near(YawRadians(65536 + 16384), YawRadians(16384)) && Near(YawRadians(-16384), YawRadians(49152)));
  // The stand spot is the summon offset behind and below the target.
  const Spot s = SummonStand({1100, 200, 50}, 0);
  CHECK(Near(s.x, 1100 - kSummonAhead) && Near(s.y, 200) && Near(s.z, 50 - kSummonUp));
  CHECK(BugItGo({1, 2.5f, 3}, 16384) == "BugItGo 1.0 2.5 3.0 0 16384 0");
  // Kill bookkeeping ignores hits after death.
  Kill k;
  Note(k, 24, false, false), Note(k, 96, true, false), Note(k, 80, false, true), Note(k, 80, true, false);
  CHECK(k.hits == 3 && k.head == 1 && k.dead && Near(k.damage, 200));
  if (failures) return 1;
  std::puts("range_math_test: ok");
  return 0;
}
