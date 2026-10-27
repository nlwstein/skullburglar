// Pure maths for the controller settings (no game or SDK types, so tests/input_math_test.cpp can run it alone).
//
// The game turns the right stick into camera speed in three steps:
//   1. a binding dead zone per axis: out = sign(v) * max(0, |v| - 0.2) / 0.8
//   2. C1PlayerInput.PreProcessInput's response curve (yaw is squared) and acceleration
//   3. the camera speed scale.
// RemapRightStick pre-compensates step 1, so a smaller dead zone can be offered without touching the game's code.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bots::input {

// The game's own dead zone on each right-stick axis (Coalesced.ini, DeadZone=0.2 on aTurn and aLookup).
inline constexpr double kGameStickDeadZone = 0.2;

struct Stick {
  int16_t x = 0, y = 0;
  bool operator==(const Stick&) const = default;
};

namespace detail {

inline int16_t ToRaw(double v) {
  return static_cast<int16_t>(std::clamp(std::lround(v * 32768.0), -32767L, 32767L));
}

// What to feed the game's per-axis dead zone so that it returns `a` (a in -1..1): its max(0, |v| - 0.2) / 0.8
// inverted. A zero axis stays zero (the game outputs zero for it too).
inline double Precompensate(double a) {
  if (a == 0) return 0;
  const double m = kGameStickDeadZone + (1.0 - kGameStickDeadZone) * std::fabs(a);
  return a < 0 ? -m : m;
}

}  // namespace detail

// Re-maps the right stick so the dead zone the player ends up with is `dead_zone` (0..0.2, a fraction of full
// travel) instead of the game's 20% per axis. At 0.2 or more the stick passes through untouched (the game as
// shipped). Below it: a round dead zone of that radius with the remaining travel rescaled to full, then the game's
// own per-axis dead zone cancelled out. Raw XINPUT units, +-32767.
inline Stick RemapRightStick(Stick in, double dead_zone) {
  if (!(dead_zone < kGameStickDeadZone - 1e-9)) return in;
  const double d = std::max(0.0, dead_zone);
  const double nx = in.x / 32768.0, ny = in.y / 32768.0;
  const double m = std::hypot(nx, ny);
  if (m <= d || m == 0) return {};
  const double scale = std::min(1.0, (m - d) / (1.0 - d)) / m;
  return {detail::ToRaw(detail::Precompensate(nx * scale)), detail::ToRaw(detail::Precompensate(ny * scale))};
}

// What the game's binding does with one axis after the stick remap, for tests: max(0, |v| - 0.2) / 0.8, signed.
inline double GameAxisDeadZone(int16_t raw) {
  const double v = raw / 32768.0;
  const double m = std::max(0.0, std::fabs(v) - kGameStickDeadZone) / (1.0 - kGameStickDeadZone);
  return v < 0 ? -m : m;
}

// C1PlayerInput's yaw response: RawJoyLookRight ^ TurnPower, keeping the sign. TurnPower 2 is the shipped
// curve, 1 is linear.
inline double YawResponse(double stick, double turn_power) {
  const double m = std::pow(std::fabs(stick), turn_power);
  return stick < 0 ? -m : m;
}

// The value written to C1PlayerInput.LagAmount for "Stick acceleration off". It is NOT 0 or 1:
// LagProportion = 1 - LagAmount^(-dt * LagTimeScale * 100), so LagAmount 1 gives a proportion of 0 and the camera
// would never turn. A huge value gives a proportion of 1: the camera follows the stick at once.
inline constexpr float kNoLagAmount = 1.0e20f;

// LagProportion for the given values (what the script computes each frame), for tests.
inline double LagProportion(double lag_amount, double dt, double lag_time_scale) {
  return 1.0 - std::pow(lag_amount, -dt * lag_time_scale * 100.0);
}

}  // namespace bots::input
