// Launcher text and layout size (B-348). The SDK's overlay font is a 10 px TrueType face (ProggyTiny) and the launcher's
// layout is in fixed pixels, so on a 1080p laptop screen the launcher came out tiny whatever Windows' display scaling
// said. The scale follows the height the launcher is drawn in, after the display scaling the SDK already applies (the
// drawer's logical size is physical size * 96 / DPI): about 1.5 at 1080 physical pixels at any scaling setting, more on
// taller screens. BOTS_UI_SCALE overrides it (tests, or a player who wants it larger or smaller).
// Plain C++ so tests/ui_scale_test.cpp can run it alone.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bots::setup {

// The height the layout is designed for: at this many logical pixels the scale is 1.
inline constexpr float kUiScaleDesignHeight = 720.0f;
inline constexpr float kUiScaleMin = 1.0f;
inline constexpr float kUiScaleMax = 3.0f;

// `logical_height`: ImGui's display height (the window's physical height * 96 / DPI). `override_text`: BOTS_UI_SCALE.
inline float LauncherUiScale(float logical_height, const char* override_text = nullptr) {
  if (override_text && *override_text) {
    char* end = nullptr;
    const float v = std::strtof(override_text, &end);
    if (end != override_text && v > 0.0f) return std::clamp(v, 0.5f, 4.0f);
  }
  if (!(logical_height > 0.0f)) return 1.0f;
  const float raw = std::clamp(logical_height / kUiScaleDesignHeight, kUiScaleMin, kUiScaleMax);
  return std::round(raw * 20.0f) / 20.0f;  // steps of 0.05: no flicker, and no odd sizes while a window is dragged
}

}  // namespace bots::setup
