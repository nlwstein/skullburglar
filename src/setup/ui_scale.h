// Launcher text and layout size (B-348). The SDK's overlay font is a 10 px TrueType face (ProggyTiny) and the launcher's
// layout is in fixed pixels, so on a 1080p laptop screen the launcher came out tiny whatever Windows' display scaling
// said. The scale follows the height the launcher is drawn in, after the display scaling the SDK already applies (the
// drawer's logical size is physical size * 96 / DPI): the text comes out about 18 px high on screen at 1080 physical pixels at any scaling setting, more on
// taller screens. BOTS_UI_SCALE overrides it (tests, or a player who wants it larger or smaller).
// Plain C++ so tests/ui_scale_test.cpp can run it alone.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bots::setup {

// The height the layout is designed for: at this many logical pixels the scale is 1.
inline constexpr float kUiScaleDesignHeight = 600.0f;
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

// The launcher window's width in the same logical pixels (B-387). The layout is designed 720 wide at scale 1; on a larger
// display the window grows with it, to 90 % of the display up to 1000 designed pixels, so long lines (folders, notes) have
// room, and never past the display's edge (20 px margin each side).
inline float LauncherWindowWidth(float logical_width, float ui_scale) {
  const float designed = 720.0f * ui_scale;
  const float wide = std::min(logical_width * 0.9f, 1000.0f * ui_scale);
  return std::max(std::min(std::max(designed, wide), logical_width - 40.0f), 1.0f);
}

// The full-screen menu's scale (launcher_menu.h): a game's front end grows with the screen, so this follows the height
// (600 logical pixels = 1) like LauncherUiScale, but also the width (900 = 1: a narrow window keeps its side columns)
// and goes up to 3.6 (a 4K screen at 100% gets the same layout as 1080p, twice as large). Below 0.8 the text would get
// too small to read; the pages scroll instead. BOTS_UI_SCALE overrides it, as for LauncherUiScale.
inline constexpr float kMenuScaleMin = 0.8f;
inline constexpr float kMenuScaleMax = 3.6f;
inline float LauncherMenuScale(float logical_width, float logical_height, const char* override_text = nullptr) {
  if (override_text && *override_text) {
    char* end = nullptr;
    const float v = std::strtof(override_text, &end);
    if (end != override_text && v > 0.0f) return std::clamp(v, 0.5f, 4.0f);
  }
  if (!(logical_height > 0.0f) || !(logical_width > 0.0f)) return 1.0f;
  const float raw = std::clamp(std::min(logical_height / kUiScaleDesignHeight, logical_width / 900.0f), kMenuScaleMin,
                               kMenuScaleMax);
  return std::round(raw * 20.0f) / 20.0f;
}

}  // namespace bots::setup
