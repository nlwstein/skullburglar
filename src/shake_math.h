// Pure maths for the screen shake setting (no game or SDK types, so tests/shake_math_test.cpp can run it alone).
#pragma once

#include <cmath>
#include <cstdint>

namespace bots::shake {

// The setting is a percentage (100 = the game's own shake, 0 = none); returns the factor, clamped to 0..1 so a
// hand-edited config can't make the shake stronger or invert it. NaN reads as 100%.
inline float Factor(double percent) {
  if (std::isnan(percent)) return 1.0f;
  if (percent <= 0.0) return 0.0f;
  if (percent >= 100.0) return 1.0f;
  return float(percent / 100.0);
}

// A shake adds an offset to the camera: `before` is the value going in, `after` the value the shake produced. Returns
// `before` plus `factor` times the offset.
inline float Scaled(float before, float after, float factor) {
  if (factor >= 1.0f) return after;
  return before + (after - before) * factor;
}

// Same for an Unreal rotation unit (a signed 32-bit int, 65536 per turn). The offset is taken in 32-bit wraparound
// arithmetic, so a shake that crosses the 0/65536 boundary still scales as the small offset it is.
inline int32_t Scaled(int32_t before, int32_t after, float factor) {
  if (factor >= 1.0f) return after;
  const int32_t offset = int32_t(uint32_t(after) - uint32_t(before));
  return int32_t(uint32_t(before) + uint32_t(int32_t(std::lround(double(offset) * double(factor)))));
}

}  // namespace bots::shake
