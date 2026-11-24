// Launcher screenshots without a screen (test-only; B-387). No test machine can capture the launcher's window (it is
// drawn into the game's Vulkan surface before the game starts, and macOS won't let a remote session record the
// screen), so the launcher draws itself into its own ImGui context at any size and display scaling, a small software
// rasterizer turns the ImGui triangles into pixels, and the frame is written as a PNG. The same code draws the window
// on screen, so the picture is what a player sees, at the size given. A script drives it with the same keyboard,
// mouse and controller events a player makes.
//
//   BOTS_SETUP_SHOT=<dir>            write the shots there (the launcher still opens as usual)
//   BOTS_SETUP_SHOT_SIZES=<list>     sizes, comma-separated: WxH or WxH@scale for display scaling (3840x2160@2 is a
//                                    4K screen at 200%); default: the window's own size and scaling
//   BOTS_SETUP_SCRIPT=<steps>        what to do, steps separated by ';' (or @<file>, one step per line); default:
//                                    one shot named "launcher" of the first page
//   BOTS_SETUP_SHOT_QUIT=1           quit when the shots are written
//   BOTS_SETUP_SHOT_SAVE=1           then save the script's changes to bots.toml as Play would (tests of what is
//                                    saved; otherwise the shots never change the settings)
//
// Steps:
//   shot:<name>             write <dir>/<name>-<W>x<H>[@<scale>].png (and a line in <dir>/shots.txt with what the
//                           layout check saw: cut-off text, dropped prompts, focus in view, smallest text)
//   key:<Name>              press and release a key (ImGui key names: Down, Enter, Escape, Tab, Q, Delete, ...);
//                           key:Shift+Right, key:Ctrl+Enter hold a modifier
//   hold:<Name>:<seconds>   hold a key (Escape held for a second cancels a key binding)
//   pad:<Button>            press and release a controller button: A B X Y LB RB LT RT Start Back Up Down Left Right
//   move:<x>,<y>            move the mouse (0..1 = a fraction of the window, more = logical pixels)
//   click:<x>,<y>           move there and click the left button; rclick: the right one
//   drag:<x1>,<y1>,<x2>,<y2> press at one point, move to the other, release
//   wheel:<notches>         turn the wheel (negative: down)
//   text:<text>             type text (into the path field, once it has the keyboard)
//   page:<name>             open a page directly: main graphics hdr game controls coop files updates about bindings
//   focus:<cvar>            select a setting (opens its page)
//   device:<kbm|pad>        show keyboard or controller prompts until the other kind is used
//   glyphs[:off]  a row of the Start and Back glyphs in the corner (preview only)
//   padkind:<xbox|x360|ps4|ps5>  the kind of controller connected (for its glyphs)
//   frames:<n>              draw n more frames
//   expect:<cvar>=<value>   log PASS or FAIL: the setting's value (the shot run's own copy) is that
//   expect-page:<name>      log PASS or FAIL: the page shown
//   expect-action:<action>  log PASS or FAIL: the last action the menu asked for (play, quit, install, move-saves, ...)
//   reset-confirm, move-confirm  open the Reset to defaults / Move the saves confirmation
// The settings a script changes are a copy: the shots never change what the launcher saves (unless
// BOTS_SETUP_SHOT_SAVE=1).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_menu.h"
#include "launcher_style.h"

namespace bots::setup {

struct ShotSize {
  int width = 0, height = 0;  // screen pixels
  float dpi = 1.0f;           // display scaling: 2 = 200%
  std::string Name() const;
};

// "1280x800,3840x2160@1.5" (an empty or bad entry is skipped).
std::vector<ShotSize> ParseShotSizes(const std::string& text);

struct ScriptStep {
  std::string verb, arg;
};
// Steps separated by ';' or newlines; "@<file>" reads them from a file. Blank steps and lines starting with # are
// skipped.
std::vector<ScriptStep> ParseShotScript(const std::string& text);

// Draws ImGui's triangles into an RGB image (`rgb` must hold width*height*3 bytes, the background already in it).
// `scale` maps ImGui's logical coordinates to pixels; `atlas` is the font atlas the draw data samples.
void RasterizeDrawData(const ImDrawData* data, float scale, int width, int height, const ImTextureData* atlas,
                       std::vector<uint8_t>& rgb);

// A plain PNG (RGB, stored deflate: large but simple). Returns false if the file can't be written.
bool WritePng(const std::filesystem::path& path, int width, int height, const std::vector<uint8_t>& rgb);

// One run of a script at one size, in a context of its own.
struct ShotHost {
  LauncherFontData fonts;
  // Draws one launcher frame into the current ImGui context with `menu`; returns what it asked for.
  std::function<LauncherAction(LauncherMenu& menu, const LauncherFonts& fonts, float dpi, PadKind pad)> frame;
  // A setting's value in the shot run's own copy of the settings (expect:).
  std::function<std::string(const std::string& cvar)> value_of;
  // Called before each size's run: start from the settings as they were.
  std::function<void()> reset;
  std::function<void(const std::string& line)> log;
  // Start each run from a copy of this menu (the live script's shots: the page and focus the real window is on).
  const LauncherMenu* initial_menu = nullptr;
};

// Script names, for the live script (setup_dialog.h, BOTS_SETUP_LIVE).
ImGuiKey ScriptKeyByName(const std::string& name);
ImGuiKey ScriptPadByName(const std::string& name);
bool ScriptPageByName(const std::string& name, LauncherPage* page);
std::string ScriptPageName(LauncherPage page);
const char* ScriptActionName(LauncherAction action);

// Runs `script` at every size and writes the shots into `dir`. Returns the number of failed expect: steps (and
// unwritable shots); 0 is a clean run.
int RunShotScript(const ShotHost& host, const std::filesystem::path& dir, const std::vector<ShotSize>& sizes,
                  const std::vector<ScriptStep>& script);

}  // namespace bots::setup
