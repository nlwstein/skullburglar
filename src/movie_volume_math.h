// Pure maths for the Movie volume setting (no game or SDK types, so tests/movie_volume_math_test.cpp can run it alone).
#pragma once

#include <cmath>
#include <cstddef>

namespace bots::movie {

// The setting is a percentage of the movie's own level (100 = as the game plays it); returns the linear gain, clamped
// to 0..2 so a hand-edited config can't make it louder than the slider allows. NaN reads as 100%.
inline float Factor(double percent) {
  if (std::isnan(percent)) return 1.0f;
  if (percent <= 0.0) return 0.0f;
  if (percent >= 200.0) return 2.0f;
  return float(percent / 100.0);
}

// Gentle limiter for boosted audio: samples up to kKnee (full scale = 1) pass unchanged, louder ones are bent toward
// 1.0 with tanh so they never exceed it and never hard-clip.
inline constexpr float kKnee = 0.8f;
inline float SoftLimit(float x) {
  const float a = std::fabs(x);
  if (!(a > kKnee)) return x;  // also passes NaN through untouched
  const float y = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
  return std::copysign(y, x);
}

// Applies `gain` to `n` samples in place. Gain 1 leaves them alone; a gain below 1 only scales (it can't push a
// sample past full scale); a boost goes through the limiter.
inline void Apply(float* samples, size_t n, float gain) {
  if (gain == 1.0f) return;
  if (gain < 1.0f) {
    for (size_t i = 0; i < n; ++i) samples[i] *= gain;
    return;
  }
  for (size_t i = 0; i < n; ++i) samples[i] = SoftLimit(samples[i] * gain);
}

// The same over interleaved samples (`channels` per frame) with the gain moving linearly from `from` to `to` across the
// block, so a change at a movie's start or end is a 5 ms ramp, not a click. Equal gains take the plain path.
inline void ApplyRamp(float* samples, size_t count, size_t channels, float from, float to) {
  if (from == to || channels == 0) return Apply(samples, count, to);
  const size_t frames = count / channels;
  for (size_t f = 0; f < frames; ++f) {
    const float g = from + (to - from) * float(f + 1) / float(frames);
    Apply(samples + f * channels, channels, g);
  }
}

}  // namespace bots::movie
