// Keep the display on while the game runs (Windows). Gamepad input doesn't count as user activity, so without this
// Windows turns the display off (and the presenter loses its swapchain) after the power plan's idle timeout during a
// controller-only session or an unattended benchmark. ES_CONTINUOUS ties the request to the calling thread; static
// initialisers run on the main thread, which lives as long as the process. The system may still sleep on lid close or
// the power button.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
const bool g_display_awake = [] {
  return SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) != 0;
}();
}  // namespace
#endif
