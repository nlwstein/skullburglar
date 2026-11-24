// The loading screen: what the player sees between pressing Play and the game's first frame. It is the launcher's own
// look (Inter and Oswald, the dusk backdrop, the gold accent: launcher_style.h) with real progress: the three steps the
// start goes through (starting up, preparing shaders with a count and a time estimate, loading the game), a bar that only
// moves forward, and on the first start a line saying why it takes a while. Drawing only; loading_logic.h works out the
// numbers and src/shader_progress.h feeds it from the game. Needs ImGui and nothing from the SDK, so
// tools/launcher_preview.cpp (--loading) draws it without the game, and BOTS_LOADING_SHOT writes PNGs of a real start.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_shot.h"
#include "launcher_style.h"
#include "loading_logic.h"

namespace bots::setup {

// Everything one frame of the screen shows.
struct LoadingView {
  LoadPhase phase = LoadPhase::kStarting;
  LoadingSteps steps;
  float bar = 0.0f;          // 0..1
  bool determinate = false;  // false: a moving stripe instead of a fraction (nothing to count)
  uint32_t done = 0, total = 0;
  bool show_counts = false;  // "812 / 2,686" on the shader step
  double remaining_seconds = -1;
  bool first_start_hint = false;
  std::string version;
  double time = 0;           // seconds, for the animation
  float dpi = 1.0f;          // screen pixels per logical pixel
  float alpha = 1.0f;        // 1 = opaque; the hand-over fades it to 0
};

LoadingView MakeLoadingView(const LoadingProgress& progress, const std::string& version, double time, float dpi,
                            float alpha);

// Draws the screen over the whole display into the current ImGui frame (its own window, no background of its own).
// Without ready `fonts` it uses ImGui's current font.
void DrawLoadingScreen(const LoadingView& view, const LauncherFonts& fonts);

// What a layout pass saw, as for the menu: text that didn't fit, and whether everything is inside the display.
struct LoadingLayoutReport {
  int truncated = 0;
  bool inside = true;
  float min_text_px = 1e9f;
};
const LoadingLayoutReport& LastLoadingReport();

// The pictures a test wants: the states of one start, from nothing counted yet through a build to the hand-over.
struct LoadingScenario {
  std::string name;
  LoadingView view;
};
std::vector<LoadingScenario> LoadingScenarios(const std::string& version);

// One PNG of `view` at `size` (a context of its own: safe to call from inside a frame, the current context is put back).
// Returns false if the file couldn't be written.
bool WriteLoadingShot(const LauncherFontData& fonts, const std::filesystem::path& file, const ShotSize& size,
                      LoadingView view);

// Every scenario at every size into `dir` as <scenario>-<size>.png, a line per shot in <dir>/shots.txt. Returns the number
// of shots with a layout problem or that couldn't be written.
int WriteLoadingScenarioShots(const LauncherFontData& fonts, const std::filesystem::path& dir,
                              const std::vector<ShotSize>& sizes, const std::string& version,
                              const std::function<void(const std::string&)>& log);

}  // namespace bots::setup
