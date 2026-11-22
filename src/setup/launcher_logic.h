// Pure rules behind the launcher's menu (launcher_menu.h): which page each setting is on, how a slider steps, how a
// choice moves, which button prompts to show and how a key binding reads. No ImGui and no SDK, so
// tests/launcher_logic_test.cpp builds it alone.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "../prompt_state.h"
#include "settings.h"

namespace bots::setup {

// The main menu's sections, in menu order. kBindings opens from the Controls page; it isn't in the menu.
enum class LauncherPage {
  kMain,
  kGraphics,
  kHdr,
  kGame,
  kControls,
  kCoop,
  kGameFiles,
  kUpdates,
  kAbout,
  kBindings,
};

struct PageInfo {
  LauncherPage page;
  const char* title;  // heading and menu entry
  const char* tab;    // short name in the section tabs
  const char* about;  // what the page holds, shown while its menu entry is selected
};

inline constexpr PageInfo kPages[] = {
    {LauncherPage::kGraphics, "Graphics", "Graphics",
     "Fullscreen, renderer, render resolution, aspect ratio, field of view, frame rate and VSync."},
    {LauncherPage::kHdr, "HDR", "HDR",
     "HDR output on an HDR display: brightness in nits, exposure and the scene's grade."},
    {LauncherPage::kGame, "Game", "Game",
     "Intro movies, achievement popups, motion blur, screen shake and movie volume."},
    {LauncherPage::kControls, "Controls", "Controls",
     "Controller and keyboard and mouse options: aim assist, stick feel, sensitivity, button prompts and key "
     "bindings."},
    {LauncherPage::kCoop, "Co-op", "Co-op", "LAN co-op, split screen and two-window co-op on one computer."},
    {LauncherPage::kGameFiles, "Game files", "Game files",
     "Where the game files are, and installing them from your own disc image."},
    {LauncherPage::kUpdates, "Updates", "Updates", "Check for new versions and install them."},
    {LauncherPage::kAbout, "About & logs", "About",
     "Version, where your settings and logs are kept, diagnostic logging and Reset to defaults."},
};

inline const PageInfo* FindPage(LauncherPage page) {
  for (const auto& p : kPages)
    if (p.page == page) return &p;
  return nullptr;
}

// The page before or after `page` in the tabs (LB / RB, Q / E), wrapping around. The key bindings count as Controls.
inline LauncherPage NeighborPage(LauncherPage page, int direction) {
  if (page == LauncherPage::kBindings) page = LauncherPage::kControls;
  constexpr int n = int(sizeof(kPages) / sizeof(kPages[0]));
  int at = 0;
  for (int i = 0; i < n; ++i)
    if (kPages[i].page == page) at = i;
  return kPages[((at + direction) % n + n) % n].page;
}

// Which page shows a setting. Every setting lands on exactly one page: one this list doesn't name (a setting added
// later) goes by its catalog section, and a setting with no section to Game, so nothing is ever left out.
inline LauncherPage PageOf(const Setting& s, std::string_view section) {
  static constexpr std::pair<const char*, LauncherPage> kByCvar[] = {
      {"fullscreen", LauncherPage::kGraphics},     {"renderer", LauncherPage::kGraphics},
      {"render_scale", LauncherPage::kGraphics},   {"aspect_ratio", LauncherPage::kGraphics},
      {"fov_offset", LauncherPage::kGraphics},     {"framerate_limit", LauncherPage::kGraphics},
      {"present_vsync", LauncherPage::kGraphics},  {"fps_counter", LauncherPage::kGraphics},
      {"intro_movies", LauncherPage::kGame},       {"achievement_popups", LauncherPage::kGame},
      {"motion_blur", LauncherPage::kGame},        {"screen_shake", LauncherPage::kGame},
      {"movie_volume", LauncherPage::kGame},       {"check_updates", LauncherPage::kUpdates},
      {"diagnostic_logging", LauncherPage::kAbout},
      // Settings from open merge requests (anti-aliasing, the pre-1.0 renderer's visibility test, toggle aim): placed
      // here so they land on the right page whichever merges first.
      {"anti_aliasing", LauncherPage::kGraphics},  {"sharpening", LauncherPage::kGraphics},
      {"pre1_occlusion", LauncherPage::kGraphics}, {"aim_toggle", LauncherPage::kControls},
  };
  for (const auto& [cvar, page] : kByCvar)
    if (s.cvar == cvar) return page;
  if (s.binding) return LauncherPage::kBindings;
  if (s.cvar.starts_with("vulkan_hdr") || section == "HDR") return LauncherPage::kHdr;
  if (section == "Controller" || section == "Keyboard and mouse") return LauncherPage::kControls;
  if (section == "Co-op") return LauncherPage::kCoop;
  if (section == "Display and launcher") return LauncherPage::kGraphics;
  if (section.empty()) return LauncherPage::kGraphics;  // the catalog's first block: display options
  return LauncherPage::kGame;
}

// The catalog's sections run on from the setting that starts them (Setting::section); this gives each setting its own.
inline std::vector<std::string> SectionsOf(const std::vector<Setting>& settings) {
  std::vector<std::string> out;
  std::string current;
  for (const auto& s : settings) {
    if (!s.section.empty()) current = s.section;
    out.push_back(current);
  }
  return out;
}

// Setting values compare as text, or as numbers when both read as one ("1.000000" is "1"); settings_logic.h's
// SameValue, without its toml++ include.
inline bool ValuesEqual(const std::string& a, const std::string& b) {
  if (a == b) return true;
  if (a.empty() || b.empty()) return false;
  char* end_a = nullptr;
  char* end_b = nullptr;
  const double x = std::strtod(a.c_str(), &end_a);
  const double y = std::strtod(b.c_str(), &end_b);
  return *end_a == 0 && *end_b == 0 && std::abs(x - y) < 1e-6;
}

// ---- Sliders ----

// A slider's text value, as the window stores it (the old launcher's rule: three decimals for middle gray, else two).
inline std::string FormatSliderValue(const Setting& s, double v) {
  char text[32];
  std::snprintf(text, sizeof(text), s.cvar == "vulkan_hdr_middle_gray" ? "%.3f" : "%.2f", v);
  return text;
}

// The value as the player reads it (the setting's own printf format: "+10 degrees", "50%", "1.50x").
inline std::string DisplaySliderValue(const Setting& s, double v) {
  char text[64];
  std::snprintf(text, sizeof(text), s.slider_format.c_str(), v);
  return text;
}

// Logarithmic sliders (the default for a slider) step by a factor; a range from 0 can't, so it steps linearly.
inline bool SliderIsLog(const Setting& s) { return !s.slider_linear && s.slider_min > 0.0; }

// One small step of a linear slider: about a fiftieth of its range, rounded to 1, 2 or 5 times a power of ten
// (field of view 0-40: 1 degree; peak brightness 200-4000: 100 nits; contrast 0.25-2: 0.05).
inline double SliderStep(const Setting& s) {
  const double raw = (s.slider_max - s.slider_min) / 50.0;
  if (!(raw > 0.0)) return 0.0;
  const double p = std::pow(10.0, std::floor(std::log10(raw)));
  const double m = raw / p;
  return (m < 1.5 ? 1.0 : m < 3.5 ? 2.0 : m < 7.5 ? 5.0 : 10.0) * p;
}

// The value `steps` small steps away (negative: down), clamped to the range. A big step (trigger, Shift) is 5 small
// ones. Linear sliders land on whole steps, so stepping never leaves an odd value like 0.37 behind; logarithmic ones
// move by a fixed factor (a fiftieth of the range in log space) and keep two decimals.
inline double StepSlider(const Setting& s, double value, int steps) {
  if (steps == 0) return std::clamp(value, s.slider_min, s.slider_max);
  double v;
  if (SliderIsLog(s)) {
    const double factor = std::pow(s.slider_max / s.slider_min, 1.0 / 50.0);
    v = std::max(value, s.slider_min) * std::pow(factor, double(steps));
    v = std::round(v * 100.0) / 100.0;
    // Rounding can undo a step near the bottom of the range: move at least one hundredth.
    if (steps > 0 && v <= value) v = value + 0.01;
    if (steps < 0 && v >= value) v = value - 0.01;
  } else {
    const double step = SliderStep(s);
    if (!(step > 0.0)) return value;
    const double k = std::round((value - s.slider_min) / step);
    v = s.slider_min + (k + steps) * step;
  }
  return std::clamp(v, s.slider_min, s.slider_max);
}

// A value from the mouse, put on the slider's grid: whole steps for a linear slider, two decimals for a log one.
inline double SnapSlider(const Setting& s, double v) {
  v = std::clamp(v, s.slider_min, s.slider_max);
  if (SliderIsLog(s)) return std::clamp(std::round(v * 100.0) / 100.0, s.slider_min, s.slider_max);
  const double step = SliderStep(s);
  if (!(step > 0.0)) return v;
  return std::clamp(s.slider_min + std::round((v - s.slider_min) / step) * step, s.slider_min, s.slider_max);
}

// Where a value sits on the slider's track, 0..1 (logarithmic sliders spread their range in log space), and back.
inline double SliderFraction(const Setting& s, double v) {
  if (!(s.slider_max > s.slider_min)) return 0.0;
  v = std::clamp(v, s.slider_min, s.slider_max);
  if (SliderIsLog(s)) return std::log(v / s.slider_min) / std::log(s.slider_max / s.slider_min);
  return (v - s.slider_min) / (s.slider_max - s.slider_min);
}
inline double SliderFromFraction(const Setting& s, double f) {
  f = std::clamp(f, 0.0, 1.0);
  if (SliderIsLog(s)) return s.slider_min * std::pow(s.slider_max / s.slider_min, f);
  return s.slider_min + f * (s.slider_max - s.slider_min);
}

// ---- Choices ----

// The index of the current value in the choices, or -1 (a value set by hand in bots.toml that isn't in the list).
inline int ChoiceIndex(const Setting& s) {
  for (int i = 0; i < int(s.choices.size()); ++i)
    if (s.choices[i].value == s.value) return i;
  return -1;
}

// The choice `direction` away; clamped at the ends (left/right on a list stops at its end, as in the game's menus)
// unless `wrap` (A / Enter / a click cycles through the list). An unlisted value moves to the first or last choice.
inline std::string StepChoice(const Setting& s, int direction, bool wrap) {
  const int n = int(s.choices.size());
  if (n == 0 || direction == 0) return s.value;
  const int at = ChoiceIndex(s);
  int next;
  if (at < 0) {
    next = direction > 0 ? 0 : n - 1;
  } else if (wrap) {
    next = ((at + direction) % n + n) % n;
  } else {
    next = std::clamp(at + direction, 0, n - 1);
  }
  return s.choices[next].value;
}

// The label of a value: the choice's label, On / Off, or the slider's text.
inline std::string ValueLabel(const Setting& s, const std::string& value) {
  if (s.slider_max > 0) return DisplaySliderValue(s, std::strtod(value.c_str(), nullptr));
  if (s.choices.empty() && !s.binding) return value == "true" ? "On" : "Off";
  for (const auto& c : s.choices)
    if (c.value == value) return c.label;
  return value;
}

// ---- Button prompts ----

enum class LastInput { kKeyboardMouse, kController };
enum class PadKind { kNone, kXbox, kPlayStation4, kPlayStation5 };  // kXbox: any pad that isn't a PlayStation one

// Which glyphs the launcher's prompts use, by the game's own rule for in-game prompts (mnk_input.h, B-105/B-160): the
// device used last wins; a controller's prompts follow pad_prompts, or with "auto" the kind of pad connected.
// (kbm_prompts only decides whether keyboard prompts replace the controller's in game; the launcher always shows keys
// while you use the keyboard or mouse.)
inline PromptDevice LauncherPromptDevice(LastInput last, PadKind pad, std::string_view pad_prompts) {
  if (last == LastInput::kKeyboardMouse) return PromptDevice::kKeyboardMouse;
  if (pad_prompts == "xbox") return PromptDevice::kXbox;
  if (pad_prompts == "ps4") return PromptDevice::kPlayStation4;
  if (pad_prompts == "ps5") return PromptDevice::kPlayStation5;
  if (pad == PadKind::kPlayStation4) return PromptDevice::kPlayStation4;
  if (pad == PadKind::kPlayStation5) return PromptDevice::kPlayStation5;
  return PromptDevice::kXbox;
}

// ---- Key bindings ----

// The keys of a binding (kbm_* cvars: comma-separated key names).
inline std::vector<std::string> SplitKeys(const std::string& value) {
  std::vector<std::string> keys;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(',', start);
    if (end == std::string::npos) end = value.size();
    std::string key = value.substr(start, end - start);
    key.erase(0, key.find_first_not_of(' '));
    key.erase(key.find_last_not_of(' ') + 1);
    if (!key.empty()) keys.push_back(key);
    start = end + 1;
  }
  return keys;
}

// A key as the player reads it on a key cap ("Return" is the Enter key; LMB is the left mouse button).
inline std::string KeyLabel(std::string_view key) {
  static constexpr std::pair<const char*, const char*> kNames[] = {
      {"Return", "Enter"},       {"NumpadEnter", "Num Enter"}, {"Escape", "Esc"},          {"Backspace", "Bksp"},
      {"Control", "Ctrl"},       {"LMB", "Left mouse"},        {"RMB", "Right mouse"},     {"MMB", "Middle mouse"},
      {"WheelUp", "Wheel up"},   {"WheelDown", "Wheel down"},  {"PageUp", "Page up"},      {"PageDown", "Page down"},
      {"CapsLock", "Caps"},      {"Backtick", "`"},            {"Minus", "-"},             {"Plus", "="},
      {"Comma", ","},            {"Period", "."},              {"Semicolon", ";"},         {"Slash", "/"},
      {"Backslash", "\\"},       {"LBracket", "["},            {"RBracket", "]"},          {"Quote", "'"},
      {"Delete", "Del"},         {"Insert", "Ins"},
  };
  for (const auto& [name, label] : kNames)
    if (key == name) return label;
  if (key.starts_with("Numpad") && key.size() == 7) return "Num " + std::string(key.substr(6));
  return std::string(key);
}

// Adds a key to a binding, or makes it the only one; a key already there isn't added twice.
inline std::string BindKey(const std::string& value, const std::string& key, bool append) {
  if (!append || value.empty()) return key;
  for (const auto& k : SplitKeys(value))
    if (k == key) return value;
  return value + "," + key;
}

// The menu counts the update's stages from the download (0 downloading, 1 verifying, 2+ installing). The installer's own
// UpdateStage enum starts at kIdle = 0, kDownloading = 1, so its value is shifted down by `downloading` (e2e 2026-11-21: with
// the raw value the menu never drew the progress bar or Cancel, and "Checking the download" showed while it downloaded).
inline int MenuUpdateStage(int installer_stage, int downloading) { return installer_stage < downloading ? 0 : installer_stage - downloading; }

}  // namespace bots::setup
