// Plain-assert tests for src/vehicle_fps_cap.h. Run: c++ -std=c++20 -I src tests/vehicle_fps_cap_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "vehicle_fps_cap.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using bots::fps::AboveVehicleLimit;
using bots::fps::VehicleCap;

int main() {
  CHECK(!AboveVehicleLimit(30));
  CHECK(!AboveVehicleLimit(60));
  CHECK(AboveVehicleLimit(90));
  CHECK(AboveVehicleLimit(144));
  CHECK(AboveVehicleLimit(0));  // unlimited

  {  // 120: drops to 60 on entering, goes back on leaving, nothing in between.
    VehicleCap c;
    CHECK(!c.Update(120, false, true).write);
    auto s = c.Update(120, true, true);
    CHECK(s.write && s.value == 60 && c.capped());
    CHECK(!c.Update(60, true, true).write);
    CHECK(!c.Update(60, true, true).write);
    s = c.Update(60, false, true);
    CHECK(s.write && s.value == 120 && !c.capped());
    CHECK(!c.Update(120, false, true).write);
  }
  {  // Unlimited comes back as 0.
    VehicleCap c;
    CHECK(c.Update(0, true, true).value == 60);
    auto s = c.Update(60, false, true);
    CHECK(s.write && s.value == 0);
  }
  {  // At or below 60 the player's value stands.
    VehicleCap c;
    CHECK(!c.Update(60, true, true).write);
    CHECK(!c.Update(30, true, true).write);
    CHECK(!c.capped());
  }
  {  // Switched off: never writes, even in a vehicle.
    VehicleCap c;
    CHECK(!c.Update(144, true, false).write);
    CHECK(!c.capped());
  }
  {  // Switched off while capped: the player's value returns.
    VehicleCap c;
    CHECK(c.Update(90, true, true).write);
    auto s = c.Update(60, true, false);
    CHECK(s.write && s.value == 90);
  }
  {  // Changed from outside while capped: that is the new player value, applied as it is.
    VehicleCap c;
    CHECK(c.Update(120, true, true).write);
    CHECK(!c.Update(30, true, true).write);  // set to 30: below 60, no cap needed
    CHECK(!c.capped() && c.user_limit() == 30);
    CHECK(!c.Update(30, false, true).write);
    CHECK(c.Update(144, true, true).value == 60);  // later raised to 144, driving: capped again
    CHECK(c.Update(60, false, true).value == 144);
  }
  std::printf(failures ? "%d failures\n" : "ok\n", failures);
  return failures ? 1 : 0;
}
