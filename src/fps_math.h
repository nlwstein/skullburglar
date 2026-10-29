// Pure maths for the in-game FPS counter (no game or SDK types, so tests/fps_math_test.cpp can run it alone).
#pragma once

#include <cstdint>

namespace bots::fps {

// Averages a running frame count over windows of at least `window_s` seconds, so the number is readable instead of
// flickering every frame. Feed it (total frames so far, time in seconds) as often as you like; it updates its answer
// once a window has elapsed.
class Meter {
 public:
  explicit Meter(double window_s = 0.5) : window_s_(window_s) {}

  // Returns true when the readout changed.
  bool Update(uint64_t frames, double now_s) {
    if (!started_ || frames < frames_ || now_s < start_s_) {  // first call, or the counter was reset
      started_ = true, start_s_ = now_s, frames_ = frames;
      return false;
    }
    const double dt = now_s - start_s_;
    if (dt < window_s_) return false;
    const uint64_t n = frames - frames_;
    valid_ = true;
    fps_ = double(n) / dt;
    frame_ms_ = n ? dt * 1000.0 / double(n) : 0.0;  // no frames in a whole window: fps 0, frame time unknown
    start_s_ = now_s, frames_ = frames;
    return true;
  }

  double fps() const { return fps_; }
  double frame_ms() const { return frame_ms_; }
  bool has_value() const { return valid_; }

 private:
  double window_s_;
  bool started_ = false, valid_ = false;
  double start_s_ = 0;
  uint64_t frames_ = 0;
  double fps_ = 0, frame_ms_ = 0;
};

}  // namespace bots::fps
