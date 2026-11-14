// BOTS_FRAME_TRACE=<file>: a per-frame timing trace for pacing work (frame-rate cap, VSync). One line per event, `t_us kind`:
// `s` = the guest handed a frame to the renderer (the swap the frame limiter paces), `p` = the presenter picked that
// frame up on the UI thread (the paint that waits for the display's vblank). Off unless the variable is set; header-only so a
// plain-assert test could include it later.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace bots {

class FrameTrace {
 public:
  static FrameTrace& Get() {
    static FrameTrace t;
    return t;
  }
  bool on() const { return file_ != nullptr; }
  void Mark(char kind) {
    if (!file_) return;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(mutex_);
    std::fprintf(file_, "%lld %c\n", static_cast<long long>(us), kind);
    if (++lines_ % 256 == 0) std::fflush(file_);  // the game is usually stopped by killing it
  }

 private:
  FrameTrace() {
    if (const char* path = std::getenv("BOTS_FRAME_TRACE"); path && *path) file_ = std::fopen(path, "w");
  }
  ~FrameTrace() {
    if (file_) std::fclose(file_);
  }
  std::FILE* file_ = nullptr;
  std::mutex mutex_;
  unsigned lines_ = 0;
};

}  // namespace bots
