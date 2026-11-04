// While the player drives, the host frame limiter drops to 60 (B-275, GitHub #7). Pure logic, no game or SDK types, so
// tests/vehicle_fps_cap_test.cpp can run it alone; the wiring is in game_hooks.cpp.
//
// Why: UE3 steps PhysX by exactly the frame time, and the game's vehicles drive slower the smaller the step gets (the
// mission 2 car covers 10% less ground at 120 fps, 38% less unlimited). Everything else measured the same at any rate,
// so only vehicles fall back; on foot and in turrets the player's limit applies.
//
// The limit is the framerate_limit cvar (hot reload). The player's own value is remembered and put back on leaving the
// vehicle. Nothing here writes bots.toml. A value that isn't the one this wrote means someone else changed the limit
// (the `setflag` test command); that becomes the player's value.
#pragma once

#include <cstdint>

namespace bots::fps {

inline constexpr uint32_t kVehicleLimit = 60;

// True when `limit` (framerate_limit; 0 = unlimited) lets the game run faster than vehicles can.
inline bool AboveVehicleLimit(uint32_t limit) { return limit == 0 || limit > kVehicleLimit; }

class VehicleCap {
 public:
  struct Step {
    bool write = false;   // set framerate_limit to `value`
    uint32_t value = 0;
  };

  // Call once per game tick with the limit as it reads now, whether the player's pawn is a vehicle, and whether the
  // fallback is enabled (BOTS_FPS_VEHICLE_CAP=0 turns it off).
  Step Update(uint32_t current, bool driving, bool enabled) {
    if (capped_ && current != kVehicleLimit) capped_ = false;  // changed from outside: that's the player's value now
    if (!capped_) user_ = current;
    const bool want = enabled && driving && AboveVehicleLimit(user_);
    Step step;
    if (want && !capped_) {
      capped_ = true;
      step = {true, kVehicleLimit};
    } else if (!want && capped_) {
      capped_ = false;
      step = {true, user_};
    }
    return step;
  }

  bool capped() const { return capped_; }
  uint32_t user_limit() const { return user_; }

 private:
  bool capped_ = false;
  uint32_t user_ = 0;
};

}  // namespace bots::fps
