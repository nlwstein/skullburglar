// Plain-assert tests for src/setup/ui_scale.h. Run: c++ -std=c++20 -I src tests/ui_scale_test.cpp -o /tmp/t && /tmp/t
#include "setup/ui_scale.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using bots::setup::LauncherUiScale;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
  // 1080 physical pixels at Windows 100 / 125 / 150 % scaling: logical heights 1080 / 864 / 720. The SDK already stretches
  // the logical layout by DPI/96, so the effective text size is scale * dpi: ~1.5x at every setting is the point.
  assert(Near(LauncherUiScale(1080.0f), 1.5f));
  assert(Near(LauncherUiScale(864.0f), 1.2f));
  assert(Near(LauncherUiScale(720.0f), 1.0f));
  assert(Near(1080.0f / 1.25f, 864.0f) && Near(1080.0f / 1.5f, 720.0f));
  // Small and odd sizes never shrink the layout; huge ones stop at 3x.
  assert(Near(LauncherUiScale(540.0f), 1.0f));
  assert(Near(LauncherUiScale(0.0f), 1.0f));
  assert(Near(LauncherUiScale(-5.0f), 1.0f));
  assert(Near(LauncherUiScale(2160.0f), 3.0f));
  assert(Near(LauncherUiScale(8000.0f), 3.0f));
  // Steps of 0.05.
  assert(Near(LauncherUiScale(800.0f), 1.1f));
  // The override wins, within bounds; junk falls back to the automatic value.
  assert(Near(LauncherUiScale(1080.0f, "2"), 2.0f));
  assert(Near(LauncherUiScale(1080.0f, "0.75"), 0.75f));
  assert(Near(LauncherUiScale(1080.0f, "99"), 4.0f));
  assert(Near(LauncherUiScale(1080.0f, "abc"), 1.5f));
  assert(Near(LauncherUiScale(1080.0f, "0"), 1.5f));
  assert(Near(LauncherUiScale(1080.0f, ""), 1.5f));
  std::puts("ui_scale_test ok");
  return 0;
}
