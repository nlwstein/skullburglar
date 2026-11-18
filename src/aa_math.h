// Pure helpers for the anti-aliasing and sharpening settings (no game or SDK types, so tests/aa_math_test.cpp can
// run them alone). The values go to the n1 renderer's n1_set_aa.
#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

namespace bots::aa {

// The `anti_aliasing` setting's value as n1's mode: 0 off, 1 FXAA, 2 SMAA 1x. Anything else (a typo in a hand-edited
// config) is off, so a bad value never turns a filter on.
inline uint32_t Mode(std::string_view name) {
  if (name == "fxaa") return 1;
  if (name == "smaa") return 2;
  return 0;
}

// The `sharpening` setting (0 to 1) as the strength n1 takes: clamped, NaN and negative read as off.
inline float Sharpening(double v) {
  if (std::isnan(v) || v <= 0.0) return 0.0f;
  if (v >= 1.0) return 1.0f;
  return float(v);
}

}  // namespace bots::aa
