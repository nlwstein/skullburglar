// Pure maths for the weapon kick setting (no game or SDK types, so tests/kick_math_test.cpp can run it alone).
//
// Every weapon fire mode (C1WeaponFireMode, or a subclass) has FireDriftPerShotInDeg, an InterpCurveVector: the view
// rotation, in degrees, that one shot adds to the controller, looked up by how long the trigger has been held. The
// native drift code then lets that offset decay with DriftDecayHalfLifeInSec. Scaling the curve scales both the kick and
// the recovery that follows it, by the same amount, and leaves the decay time alone.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bots::kick {

// InterpCurvePoint<FVector> in the guest: float InVal; FVector OutVal, ArriveTangent, LeaveTangent; BYTE InterpMode
// (padded to 4).
constexpr uint32_t kPointSize = 44;
constexpr uint32_t kFloatsPerPoint = 9;      // OutVal, ArriveTangent, LeaveTangent: everything except InVal
constexpr uint32_t kMaxPoints = 32;          // far above what the game ships (3 to 6 points); bigger means the layout is wrong
constexpr uint32_t kInValOffset = 0;
constexpr uint32_t kFirstScaledOffset = 4;   // the nine floats that follow InVal

// The setting is a percentage (100 = the game's own kick, 0 = none), clamped to 0..1. NaN reads as 100%.
inline float Factor(double percent) {
  if (std::isnan(percent) || percent >= 100.0) return 1.0f;
  if (percent <= 0.0) return 0.0f;
  return float(percent / 100.0);
}

// True when `n` points whose InVal values are `in_vals` look like a curve we may rewrite: a plausible count, finite
// numbers, and InVal never decreasing. Anything else is left alone (a wrong guess about the layout must not corrupt
// memory).
inline bool PlausibleCurve(const std::vector<float>& in_vals) {
  if (in_vals.empty() || in_vals.size() > kMaxPoints) return false;
  for (size_t i = 0; i < in_vals.size(); ++i) {
    if (!std::isfinite(in_vals[i]) || std::abs(in_vals[i]) > 1e4f) return false;
    if (i && in_vals[i] < in_vals[i - 1]) return false;
  }
  return true;
}

// The value to write for a shipped value `original`: scaled, and exactly `original` at 100% so the default is untouched.
inline float Scaled(float original, float factor) { return factor >= 1.0f ? original : original * factor; }

}  // namespace bots::kick
