// Pure rules behind the launcher's menu (launcher_menu.h): which page each setting is on, how a slider steps, how a
// choice moves, which button prompts to show and how a key binding reads. No ImGui and no SDK, so
// tests/launcher_logic_test.cpp builds it alone.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "../prompt_state.h"
#include "settings.h"

namespace bots::setup {

// The main menu's sections (enum LauncherPage, settings.h), in menu order (kPages). kBindings opens from the Controls page.
struct PageInfo {
  LauncherPage page;
  const char* title;  // heading and menu entry
  const char* tab;    // short name in the section tabs
  const char* about;  // what the page holds, shown while its menu entry is selected
};

inline constexpr PageInfo kPages[] = {
    {LauncherPage::kGraphics, "Graphics", "Graphics",
     "Fullscreen, aspect ratio, field of view, render resolution, anti-aliasing, frame rate, VSync and the renderer."},
    {LauncherPage::kHdr, "HDR", "HDR",
     "HDR output on an HDR display: brightness in nits, exposure and the scene's grade."},
    {LauncherPage::kGame, "Game", "Game",
     "Intro movies and movie volume, motion blur and screen shake, achievement popups."},
    {LauncherPage::kControls, "Controls", "Controls",
     "Aiming, controller and keyboard and mouse options: stick feel, sensitivity, button prompts and key bindings."},
    {LauncherPage::kCoop, "Co-op", "Co-op", "LAN co-op over your network; split screen and two-window co-op on one computer."},
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

// Pages the catalog may use for settings. Every catalog setting names its page and an optional group heading
// (settings_catalog.cpp), so there is no guessing from a section name or a cvar list; the catalog test fails for a
// setting that names none.
inline bool PageTakesSettings(LauncherPage page) {
  return page != LauncherPage::kMain && page != LauncherPage::kGameFiles;
}

// The group headings of `page`, in the order they show: first appearance in the catalog, ungrouped settings count as
// the group "". Returns false if a group's settings are not together (the heading would show twice).
inline bool PageGroups(const std::vector<Setting>& settings, LauncherPage page, std::vector<std::string>* groups) {
  std::vector<std::string> out;
  bool together = true;
  for (const auto& s : settings) {
    if (s.page != page) continue;
    if (out.empty() || out.back() != s.group) {
      if (std::find(out.begin(), out.end(), s.group) != out.end()) together = false;
      out.push_back(s.group);
    }
  }
  if (groups) *groups = out;
  return together;
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
// kXbox: any pad that isn't one of the others (Xbox One, Series, Steam Deck, generic). kXbox360: SDL says Xbox 360.
enum class PadKind { kNone, kXbox, kXbox360, kPlayStation4, kPlayStation5 };

// The pad families SDL tells apart that change the glyphs (SDL_GetGamepadType, mapped in pad_nav.h; kept here so the
// mapping has a unit test without SDL).
enum class PadType { kOther, kXbox360, kPlayStation3, kPlayStation4, kPlayStation5 };

inline PadKind PadKindForType(PadType type) {
  switch (type) {
    case PadType::kXbox360: return PadKind::kXbox360;
    case PadType::kPlayStation3:  // no glyphs of its own: the DualShock 4's
    case PadType::kPlayStation4: return PadKind::kPlayStation4;
    case PadType::kPlayStation5: return PadKind::kPlayStation5;
    default: return PadKind::kXbox;
  }
}

// What SDL tells us about a connected pad, for choosing its glyphs (pad_nav.h fills it in; plain data so the choice is
// unit-testable). `steam_handle`: SDL_GetGamepadSteamHandle() is non-zero, i.e. the pad comes through Steam Input.
struct PadInfo {
  PadType type = PadType::kOther;
  std::string_view name;
  uint16_t vendor = 0, product = 0;
  bool steam_handle = false;
};

// Valve's USB vendor id: the Steam Deck's own controller (0x1205) and Steam Input's virtual gamepad (0x11FF).
inline constexpr uint16_t kValveVendor = 0x28DE;

// A pad that is really Steam Input's: SDL names its virtual gamepad "Steam Virtual Gamepad" (vendor Valve), and it
// usually reports the Xbox 360 type whatever the real pad underneath is.
inline bool IsSteamPad(const PadInfo& pad) {
  if (pad.steam_handle || pad.vendor == kValveVendor) return true;
  return pad.name.find("Steam") != std::string_view::npos;
}

// Running on a Steam Deck: Steam sets SteamDeck=1 for the games it launches; the board name ("Jupiter" LCD, "Galileo" OLED,
// /sys/devices/virtual/dmi/id/product_name) covers a launch outside Steam. Either input may be empty.
inline bool IsSteamDeckHost(std::string_view steamdeck_env, std::string_view dmi_product) {
  if (steamdeck_env == "1") return true;
  while (!dmi_product.empty() && (dmi_product.back() == '\n' || dmi_product.back() == ' ')) dmi_product.remove_suffix(1);
  return dmi_product == "Jupiter" || dmi_product == "Galileo";
}

// The glyph family for a pad. 360 glyphs only for a real 360 pad: Steam Input's virtual pad reports as a 360 for any
// real pad behind it, and on a Steam Deck the Deck's own glyphs (Menu, View) are the right ones whatever SDL says.
inline PadKind PadKindFor(const PadInfo& pad, bool steam_deck) {
  if (pad.type == PadType::kXbox360 && (steam_deck || IsSteamPad(pad))) return PadKind::kXbox;
  return PadKindForType(pad.type);
}

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
  if (pad == PadKind::kXbox360) return PromptDevice::kXbox360;
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

// What the Updates page says when the update didn't install. `raw` is the installer's technical reason (it goes to the
// log as it is). A failed download ("the download failed (curl 22)", Windows: "(HTTP 404)" or "(error 12029)") reads in
// player language: no tool names or codes in the window. Anything else keeps the old sentence.
inline std::string UpdateFailureNote(const std::string& raw) {
  constexpr std::string_view kTail = " Check your connection and try again. Your install is unchanged.";
  constexpr std::string_view kHead = "the download failed (";
  if (raw.rfind(kHead, 0) != 0) {
    return "The update didn't install (" + raw +
           "). Your install is unchanged; the download page gets the new version by hand.";
  }
  const std::string detail = raw.substr(kHead.size());
  auto number_after = [&](std::string_view tag) {
    return detail.rfind(tag, 0) == 0 ? std::atoi(detail.c_str() + tag.size()) : -1;
  };
  const int curl = number_after("curl "), http = number_after("HTTP "), sys = number_after("error ");
  const std::string head = "The download failed";
  if (http >= 400 || curl == 22) return head + " (the server returned an error)." + std::string(kTail);
  if (curl == 23) return head + " (the file couldn't be saved; is the disk full?). Your install is unchanged.";
  if (sys >= 0 || curl == 6 || curl == 7 || curl == 18 || curl == 28 || curl == 35 || curl == 52 || curl == 55 ||
      curl == 56 || curl == 60)
    return head + " (the connection dropped or timed out)." + std::string(kTail);
  return head + "." + std::string(kTail);
}

// ---- Auto-save ----

// A change is written when it is a discrete one (a switch, a choice, a key binding), at once; a slider is written when
// the mouse lets go of it, or after it has been still for kAutosaveIdleSeconds (a held arrow key or a drag would
// otherwise write 60 times a second). `dirty`: some setting differs from what was last written; `slider_pending`: one of
// those is a slider; `released`: the mouse let go of a slider this frame; `idle`: seconds since the last change.
inline constexpr double kAutosaveIdleSeconds = 0.3;
inline bool AutosaveDue(bool dirty, bool slider_pending, bool released, double idle) {
  if (!dirty) return false;
  if (!slider_pending || released) return true;
  return idle >= kAutosaveIdleSeconds;
}

// How long "Saved" stays in the footer.
inline constexpr double kSavedFlashSeconds = 1.8;

// The short reason in the footer for a failed write: the settings file's own error text reads
// "Can't write <path> (<why>)"; the footer says what it means for the player.
inline std::string SaveErrorShort(const std::string& error) {
  std::string why;
  if (const size_t open = error.rfind('('); open != std::string::npos && error.back() == ')')
    why = error.substr(open + 1, error.size() - open - 2);
  auto has = [&](const char* needle) { return why.find(needle) != std::string::npos; };
  if (has("read-only") || has("Read-only") || has("Permission denied") || has("Operation not permitted") || has("Access is denied") ||
      has("denied"))
    return "the settings file or folder is read-only";
  if (has("No space") || has("disk is full") || has("not enough space") || has("Not enough space") || has("quota"))
    return "the disk is full";
  if (has("No such file") || has("cannot find the path")) return "the settings folder can't be found";
  return why.empty() ? "the settings file can't be written" : why;
}

}  // namespace bots::setup
