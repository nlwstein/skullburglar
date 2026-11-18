// Pure maths for the field-of-view setting (no game or SDK types, so tests/fov_math_test.cpp can run it alone).
#pragma once

#include <algorithm>
#include <cmath>

namespace bots::fov {

constexpr double kPi = 3.14159265358979323846;
// The player camera's horizontal field of view at 16:9 as the game ships it, in degrees (80 walking and running,
// 50 while aiming).
constexpr double kOriginal = 80.0;
// The slider's top: kOriginal + kMaxOffset degrees.
constexpr double kMaxOffset = 40.0;

// The setting is degrees added to the original horizontal field of view; clamped to 0..kMaxOffset so a hand-edited
// config can't narrow the view or make it absurd. NaN reads as 0.
inline double ClampedOffset(double offset) {
  if (std::isnan(offset)) return 0.0;
  return std::clamp(offset, 0.0, kMaxOffset);
}

// The factor on tan(half the field of view) that turns kOriginal into kOriginal + offset. Applying the same factor to
// any other field of view the game asks for (the zoom when aiming, a scope, a vehicle) keeps that view's proportion
// to the normal one. 1 for offset 0, so the game's own value is untouched.
inline double Scale(double offset) {
  const double o = ClampedOffset(offset);
  if (o <= 0.0) return 1.0;
  return std::tan((kOriginal + o) * kPi / 360.0) / std::tan(kOriginal * kPi / 360.0);
}

// Degrees in, degrees out: tan(new / 2) = tan(old / 2) * scale. A value outside (0, 180) comes back unchanged.
inline double Apply(double degrees, double scale) {
  const double half = degrees * kPi / 360.0;
  if (!(half > 0.0 && half < kPi / 2) || !(scale > 0.0) || scale == 1.0) return degrees;
  return std::atan(std::tan(half) * scale) * 360.0 / kPi;
}

}  // namespace bots::fov
