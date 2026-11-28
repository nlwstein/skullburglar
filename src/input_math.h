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

// "Stick look speed" (B-303): a multiple of the game's own stick look speed, 1 (as shipped) to 3. The game's
// C1PlayerInput scales TurnSpeed and PitchSpeed by its Controller Sensitivity option (0.5 / 1 / 1.5); the setting
// multiplies the fields behind those (and MaxPowerTurn, the full-tilt bonus that is added after TurnSpeed, so the whole
// turn scales). Anything outside the range, or not a number, is pulled back into it.
inline constexpr double kMaxLookSpeed = 3.0;
inline double ClampLookSpeed(double v) { return std::isfinite(v) ? std::clamp(v, 1.0, kMaxLookSpeed) : 1.0; }

// aTurn the script produces in steady state for a stick position (after the game's dead zone), for tests:
// stick^TurnPower * TurnSpeed * sensitivity + PowerTurn, where the look speed multiplies TurnSpeed and MaxPowerTurn.
inline double SteadyTurn(double stick, double turn_power, double turn_speed, double sensitivity, double power_turn,
                         double look) {
  return YawResponse(stick, turn_power) * turn_speed * look * sensitivity + power_turn * look;
}

// The value written to C1PlayerInput.LagAmount for "Stick acceleration off". It is NOT 0 or 1:
// LagProportion = 1 - LagAmount^(-dt * LagTimeScale * 100), so LagAmount 1 gives a proportion of 0 and the camera
// would never turn. A huge value gives a proportion of 1: the camera follows the stick at once.
inline constexpr float kNoLagAmount = 1.0e20f;

// LagProportion for the given values (what the script computes each frame), for tests.
inline double LagProportion(double lag_amount, double dt, double lag_time_scale) {
  return 1.0 - std::pow(lag_amount, -dt * lag_time_scale * 100.0);
}

// Mouse noise gate, 1.4.0 (B-269): motion accumulated since the last frame is held back, not dropped, while its
// length is below `gate` pixels, and all of it is released once it adds up. Steady slow motion passes in bursts and
// a sensor that creeps one count at a time in one direction gets through anyway, which is what a gyro that drifts
// does (measured in tests/mouse_gate_sim.cpp). Kept for that comparison; the game uses MouseGate below.
inline bool MouseGateHolds(double acc_x, double acc_y, double gate) {
  return gate > 0 && std::hypot(acc_x, acc_y) < gate;
}

// Mouse drift gate (B-269). A gyro-as-mouse that lies still still reports the odd count, mostly the same direction at
// a slow, steady rate (drift) or +1/-1 around zero (noise). Deliberate motion, even a slow nudge, comes at a higher
// rate. So the gate watches how fast counts arrive: a leaky sum of the counts, `level`, that loses its value with time
// constant kTau (a steady rate of r counts per second settles at r * kTau).
//   - Idle: counts are held and not released, they only add to `level`. When level reaches `gate` pixels the motion is
//     deliberate: the gate opens and releases the level (nearly all the counts of this move).
//   - Open: every count passes at once. The gate closes again when level falls under kClose * gate, so a pause of
//     about half a second shuts it, and the level starts again from zero.
// A drift slower than gate / kTau counts per second never reaches the gate and is dropped; +1/-1 noise cancels in
// the level. gate <= 0 passes everything, always.
struct MouseGate {
  static constexpr double kTau = 0.4;    // seconds
  static constexpr double kClose = 0.5;  // close below this share of the gate
  double close = kClose;                 // (a member so the simulation can try other values)
  double x = 0, y = 0;                   // leaky sum of counts, pixels
  bool open = false;

  // Strict mode (B-396, opt in): a sensor with wide random jitter (a DualSense Edge's gyro) has a level that wanders
  // past the gate on its own, because the sum of random counts grows like the square root of their number. What tells it
  // from a turn is direction: a turn's counts point one way, jitter's cancel. So strict mode also keeps, over a longer
  // time kTauC, the leaky sum of the counts (cx, cy) and of their lengths (path). The gate opens only when the
  // sum is at least kCoherence of the path, and shuts again when it falls under kCoherenceClose of it. A steady drift
  // or turn has sum == path, so it is unchanged; only mixed-direction motion is held back more.
  static constexpr double kTauC = 0.8;           // seconds
  static constexpr double kCoherence = 0.6;      // open needs |sum| >= this share of the path
  static constexpr double kCoherenceClose = 0.3; // an open gate closes below this share
  double cx = 0, cy = 0, path = 0;

  // Opening releases at most this many gates' worth of held-back counts on top of the frame's own (B-396, #21 second
  // report). The held level can be several gates long (a sensor that jitters keeps it high while strict mode waits for
  // a direction), and releasing all of it in one frame is a visible jump from a camera the player did not touch. The
  // part over the cap is dropped, as the drift under the gate always was. A turn or flick loses at most the cap
  // (0.5 degrees at the default gate) at its start. <= 0 releases everything, the 1.15.0 behaviour (BOTS_MOUSE_GATE_CAP=0).
  double backlog_cap = 1.0;

  // One frame: `dt` seconds since the last one, (dx, dy) the counts that arrived meanwhile. Returns what passes.
  struct Out {
    double x = 0, y = 0;
  };
  Out Step(double dx, double dy, double dt, double gate, double tau = kTau, bool strict = false) {
    if (!(gate > 0)) {
      x = y = cx = cy = path = 0, open = false;
      return {dx, dy};
    }
    const double keep = tau > 0 && dt > 0 ? std::exp(-dt / tau) : 0.0;
    x = x * keep + dx, y = y * keep + dy;
    const double level = std::hypot(x, y);
    bool coherent_open = true, coherent_shut = false;
    if (strict) {
      const double kc = dt > 0 ? std::exp(-dt / kTauC) : 0.0;
      cx = cx * kc + dx, cy = cy * kc + dy, path = path * kc + std::hypot(dx, dy);
      const double sum = std::hypot(cx, cy);
      coherent_open = sum >= kCoherence * path;
      coherent_shut = sum < kCoherenceClose * path;
    } else {
      cx = cy = path = 0;
    }
    if (open) {
      // The level now holds counts that already passed: forget them when the gate shuts, so they are not released again.
      if (level < close * gate || coherent_shut) open = false, x = y = cx = cy = path = 0;
      return {dx, dy};
    }
    if (level < gate || !coherent_open) return {};
    open = true;
    if (backlog_cap > 0) {
      // This frame's counts pass whole; the backlog before them is limited to backlog_cap * gate in length.
      const double bx = x - dx, by = y - dy, blen = std::hypot(bx, by), lim = backlog_cap * gate;
      const double k = blen > lim ? lim / blen : 1.0;
      return {dx + bx * k, dy + by * k};
    }
    return {x, y};
  }
};

// Rest deadband (#21, B-396). A mouse count turns the view kDegPerCountAtOne x sensitivity degrees (measured, B-311:
// 65536 units a turn / 60), 0.33 degrees at the default 1.0, and kAimScale of that while aiming. A gyro lying still sends
// the odd +-1 count; the noise gate above is set in counts, so what it holds back in degrees changes with the
// sensitivity. The rest deadband is the same gate (always with strict mode) with the threshold given in degrees: the
// view stays where it is until the motion adds up to this angle in one direction.
inline constexpr double kDegPerCountAtOne = 360.0 * 60.0 / 65536.0;
inline constexpr double kAimScale = 0.625;
inline constexpr double kMaxRestGateCounts = 32.0;
inline double DegPerCount(double sensitivity, bool aiming) {
  return kDegPerCountAtOne * sensitivity * (aiming ? kAimScale : 1.0);
}
// The gate threshold, in counts, for a deadband of `degrees`: 0 = off. At least one count (a deadband under one count
// would hold nothing back), at most kMaxRestGateCounts (a very fine step must not need a huge move to open).
inline double RestGateCounts(double degrees, double sensitivity, bool aiming) {
  const double per = DegPerCount(sensitivity, aiming);
  if (!(degrees > 0) || !(per > 0) || !std::isfinite(degrees)) return 0;
  return std::clamp(degrees / per, 1.0, kMaxRestGateCounts);
}

}  // namespace bots::input
