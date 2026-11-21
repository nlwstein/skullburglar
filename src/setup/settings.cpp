// See settings.h.
#include "../path_utf8.h"
#include "settings.h"

#include "../renderer_choice.h"
#include "settings_logic.h"
#include "update_check.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>

// The app's own cvar (intro movies are a UE3 command-line switch, see BotsApp::OnPreSetup).
REXCVAR_DEFINE_BOOL(intro_movies, false, "bots", "Play the Legal / THQ / Swordfish logo movies at launch");
REXCVAR_DEFINE_BOOL(achievement_popups, false, "bots",
                    "Show a popup when an achievement unlocks. Off by default: while one is on screen the frame rate "
                    "drops and stutters. Unlocks are recorded either way");
REXCVAR_DEFINE_BOOL(launcher, true, "bots",
                    "Show the launcher before starting the game (default: unless --game_data_root is given)");
REXCVAR_DEFINE_BOOL(check_updates, true, "bots",
                    "Release builds: the launcher checks online for a newer version and offers its download page");
REXCVAR_DEFINE_BOOL(diagnostic_logging, false, "bots",
                    "Start the game with BOTS_DIAG=1: a fuller log (frame hitches, periodic host reports, GPU timing "
                    "spans) for bug reports. Off by default");
REXCVAR_DEFINE_STRING(renderer, bots::kDefaultRenderer, "bots",
                      "Which renderer draws the game: new (the default; on tvOS pre1 is) or pre1, the pre-1.0 renderer "
                      "(deprecated). Only pre1 selects the old one; any other value is the new renderer");
REXCVAR_DEFINE_STRING(pre1_occlusion, "auto", "bots",
                      "Pre-1.0 renderer only: skip objects the graphics card says are hidden behind walls (auto, on, "
                      "off). auto is on, except on Intel graphics. Off draws everything in view (a BOTS_GFX_OCCLUSION "
                      "in the environment wins)");
REXCVAR_DEFINE_STRING(render_scale, "auto", "bots",
                      "Render resolution as a multiple of 720p (auto, 3, 2, 1.5, 1.25, 1, 0.75, "
                      "0.667, 0.5); auto is 2/3 on Intel GPUs, else 1");
REXCVAR_DEFINE_STRING(anti_aliasing, "smaa", "bots",
                      "Smooths jagged edges in the new renderer (off, fxaa, smaa). Read by the renderer every frame; "
                      "the pre-1.0 renderer ignores it");
REXCVAR_DEFINE_DOUBLE(sharpening, 0.0, "bots",
                      "Sharpens the picture after anti-aliasing in the new renderer (0 off to 1). The HUD is not "
                      "sharpened")
    .range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(present_vsync, true, "bots",
                    "Wait for the display's refresh to show each frame (Vulkan presentation): no tearing. Off presents "
                    "immediately, with the lowest latency, and can tear. (Not `vsync`, the emulated console's vblank.)");

REXCVAR_DEFINE_BOOL(fps_counter, false, "bots",
                    "Show the frame rate, frame time and renderer in a corner of the screen (fps_counter.h)");
REXCVAR_DEFINE_DOUBLE(vulkan_hdr_exposure, 0.0, "bots/HDR", "Scene exposure in stops");
REXCVAR_DEFINE_DOUBLE(vulkan_hdr_middle_gray, 0.18, "bots/HDR", "Scene middle gray (0.18 preserves the original)");
REXCVAR_DEFINE_DOUBLE(vulkan_hdr_contrast, 1.0, "bots/HDR", "Scene contrast around 18 percent gray");
REXCVAR_DEFINE_DOUBLE(vulkan_hdr_highlights, 1.0, "bots/HDR", "Scene highlights");
REXCVAR_DEFINE_DOUBLE(vulkan_hdr_shadows, 1.0, "bots/HDR", "Scene shadows");
REXCVAR_DEFINE_DOUBLE(vulkan_hdr_saturation, 1.0, "bots/HDR", "Scene saturation");

namespace bots::setup {
namespace fs = std::filesystem;

namespace {

std::vector<Setting> Catalog() {
  std::vector<Setting> s;
  s.push_back({"fullscreen", "", "Fullscreen", "F11 toggles it in game.", {}});
  s.push_back({"intro_movies", "", "Play intro movies", "The logo movies before the title screen.", {}});
  s.push_back({"achievement_popups", "", "Achievement popups",
               "Can make the game stutter while one shows. Unlocks are recorded either way.", {}});
  s.push_back({"renderer", "", "Renderer",
               "The new renderer draws the 3D world with the game's rebuilt scene renderer. The pre-1.0 renderer is "
               "the one earlier versions used; it stays for now in case the new one doesn't work on your PC, and will "
               "be removed. Takes effect the next time the game starts.",
               {{"new", "New renderer (recommended)"}, {"pre1", "Pre-1.0 renderer (deprecated)"}}});
  s.push_back({"pre1_occlusion", "", "Pre-1.0 renderer: hide objects behind walls",
               "Only the pre-1.0 renderer uses this. It asks the graphics card which objects are hidden behind walls "
               "and doesn't draw them. On some cards that makes posters, blood and trash on the ground flicker in "
               "sunny areas: choose Off to draw them every frame. Takes effect the next time the game starts.",
               {{"auto", "Automatic"}, {"on", "On"}, {"off", "Off (try this if decals flicker)"}}});
  s.push_back({"render_scale", "", "Render resolution",
               "Higher is sharper and costs GPU time; lower is faster on weak GPUs. Automatic is 720p, or 67% on "
               "Intel graphics.",
               {{"auto", "Automatic"}, {"3", "4K  (2160p)"}, {"2", "1440p"}, {"1.5", "1080p"},
                {"1.25", "900p"}, {"1", "720p  (original)"}, {"0.75", "75%"}, {"0.667", "67%"}, {"0.5", "50%"}}});
  s.push_back({"aspect_ratio", "", "Aspect ratio",
               "Wider than 16:9 fills an ultrawide screen and shows more at the sides; the HUD and menus stretch. "
               "Automatic matches the screen in fullscreen, or the window.",
               {{"16:9", "16:9 (original)"}, {"auto", "Automatic"}, {"21:9", "21:9"}, {"32:9", "32:9"}}});
  s.push_back({"fov_offset", "", "Field of view",
               "Degrees added to the game's horizontal view. 0 is as shipped; more shows more of the scene and makes "
               "things look smaller. The aiming zoom and scopes widen in the same proportion; cutscenes and vehicle "
               "cameras keep the original view. F6 changes it live. Ctrl-click to type a value.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 40, s.back().slider_linear = true, s.back().slider_format = "+%.0f degrees";
  s.push_back({"anti_aliasing", "", "Anti-aliasing",
               "Smooths jagged edges on the 3D scene; the HUD and text are not touched. SMAA is the cleanest; FXAA is "
               "the cheapest and a little softer. Requires the new renderer.",
               {{"off", "Off"}, {"fxaa", "FXAA (fast)"}, {"smaa", "SMAA (cleaner)"}}});
  s.push_back({"sharpening", "", "Sharpening",
               "Brings back the crispness that anti-aliasing and a lower resolution take away. 0 is off. Edges that "
               "are already sharp change little. Requires the new renderer.", {}});
  s.back().slider_min = 0, s.back().slider_max = 1, s.back().slider_linear = true, s.back().slider_format = "%.2f";
  s.push_back({"vulkan_hdr", "", "Enable HDR (restart required)",
               "Native scRGB output on an HDR display. Requires the new renderer. "
               "Enable HDR in the operating system too. F6 opens the live display controls.", {}});
  s.back().section = "HDR";
  s.push_back({"vulkan_hdr_paper_white", "", "Game brightness (paper white)",
               "The game's reference white in nits. Highlights can extend above it.", {}});
  s.back().slider_min = 80, s.back().slider_max = 500, s.back().slider_linear = true, s.back().slider_format = "%.0f nits";
  s.push_back({"vulkan_hdr_peak", "", "Peak brightness", "Set this to the display's peak brightness.", {}});
  s.back().slider_min = 200, s.back().slider_max = 4000, s.back().slider_linear = true, s.back().slider_format = "%.0f nits";
  s.push_back({"vulkan_hdr_ui_white", "", "UI brightness", "HUD, menus and host overlays in nits.", {}});
  s.back().slider_min = 40, s.back().slider_max = 500, s.back().slider_linear = true, s.back().slider_format = "%.0f nits";
  s.push_back({"vulkan_hdr_exposure", "", "Exposure", "Scene exposure in stops, before the HDR shoulder.", {}});
  s.back().slider_min = -4, s.back().slider_max = 4, s.back().slider_linear = true, s.back().slider_format = "%.2f EV";
  s.push_back({"vulkan_hdr_middle_gray", "", "Middle gray",
               "Scene midtones. 0.18 keeps the original curve; black and reference white stay fixed.", {}});
  s.back().slider_min = 0.05, s.back().slider_max = 0.5, s.back().slider_linear = true, s.back().slider_format = "%.3f";
  for (const auto& [name, label] : std::vector<std::pair<const char*, const char*>>{
         {"vulkan_hdr_contrast", "Contrast"}, {"vulkan_hdr_highlights", "Highlights"},
         {"vulkan_hdr_shadows", "Shadows"}, {"vulkan_hdr_saturation", "Saturation"}}) {
    s.push_back({name, "", label, "1.0 keeps the game's original grade. Adjusts the scene; the HUD is unchanged.", {}});
    s.back().slider_min = 0.25, s.back().slider_max = 2, s.back().slider_linear = true;
  }
  s.push_back({"motion_blur", "", "Motion blur",
               "The smear when the camera turns or things move fast. Off keeps every other effect.",
               {}});
  s.back().section = "Game";
  s.push_back({"screen_shake", "", "Screen shake",
               "How much the camera shakes from explosions, gunfire and hits, as a share of the game's own. 100% is "
               "as shipped; 0% turns it off.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 100, s.back().slider_linear = true, s.back().slider_format = "%.0f%%";
  s.push_back({"movie_volume", "", "Movie volume",
               "How loud the pre-rendered movies (story cutscenes, music videos, the title screen movie) are, as a "
               "share of their own level. 100% is as the game plays them; the slider goes up to 200%, with a soft "
               "limiter so loud parts don't distort. Game sound and music are not affected.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 200, s.back().slider_linear = true, s.back().slider_format = "%.0f%%";
  s.push_back({"framerate_limit", "", "Frame rate limit",
               "30 plays at a steady 30 fps: choose it if the game feels jumpy because your computer can't hold 60. "
               "60 is what the game was made for. 90, 120 and 144 run smoother on a high-refresh screen; the game "
               "holds 60 while you drive and while an AI-driven vehicle is in play, because vehicles drive slower at "
               "higher frame rates. Unlimited is experimental.",
               {{"30", "30 fps (steadier on weaker hardware)"}, {"60", "60 fps (original)"}, {"90", "90 fps"}, {"120", "120 fps"}, {"144", "144 fps"},
                {"0", "Unlimited (experimental)"}}});
  s.push_back({"present_vsync", "", "VSync", "Off has the lowest latency but can tear.", {}});
  s.back().section = "Display and launcher";
  s.push_back({"fps_counter", "", "Show FPS counter",
               "A small readout in the top-left corner: frames per second, milliseconds per frame, and which renderer "
               "is drawing. Handy for bug reports. Takes effect the next time the game starts.",
               {}});
  s.push_back({"check_updates", "", "Check for new versions",
               "When the launcher opens, look online for a newer Skullburglar and offer its download page.", {}});
  s.push_back({"diagnostic_logging", "", "Diagnostic logging",
               "A fuller log for bug reports: slow frames, a system report every few seconds and GPU timings. Costs a "
               "little speed and makes the log bigger. The normal log always has the essentials. Takes effect the next "
               "time the game starts.",
               {}});
  s.push_back({"pad_prompts", "", "Controller button prompts",
               "Which buttons the game shows. Automatic matches the controller you used last.",
               {{"auto", "Automatic"}, {"xbox", "Xbox"}, {"ps4", "PlayStation 4"}, {"ps5", "PlayStation 5"}}});
  s.back().section = "Controller";
  s.push_back({"aim_assist", "", "Aim assist",
               "The game slows the crosshair and pulls it toward enemies near it while you move (even through walls, "
               "as on the Xbox 360). Turn off for raw aiming. Applies to the mouse too.",
               {}});
  s.push_back({"aim_toggle", "", "Toggle aim",
               "Press Aim once to start aiming and again to stop, instead of holding it. Works with a controller and "
               "with keyboard and mouse (every key bound to Aim, such as right mouse button or Shift). Taking cover "
               "or running (A) also ends it; menus, pause and vehicles reset it.",
               {}});
  s.push_back({"right_stick_deadzone", "", "Right stick dead zone",
               "How far you push the right stick before the camera moves. 20% is the game's own, applied to each "
               "direction separately; lower values use a smaller round dead zone, so small and diagonal movements "
               "register. 0% has none, so a stick that drifts will move the camera. Steam Input's own dead zone, if "
               "you use it, still applies.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 20, s.back().slider_linear = true, s.back().slider_format = "%.0f%%";
  s.push_back({"stick_acceleration", "", "Stick acceleration",
               "The game speeds up turning after you hold the stick fully over, and eases the camera in when you "
               "push it further, as on the Xbox 360. Turn off for constant turning speed that follows the stick at "
               "once (the top turning speed is then lower).",
               {}});
  s.push_back({"stick_response", "", "Stick response",
               "How the stick maps to turning left and right. The original curve turns slowly for small movements. "
               "Linear turns in proportion to the stick, so small movements are faster. Up and down is always "
               "linear. The game's own Controller Sensitivity option still applies.",
               {{"original", "Original curve"}, {"linear", "Linear"}}});
  s.push_back({"stick_look_speed", "", "Stick look speed",
               "How fast the right stick turns the camera, as a multiple of the game's own speed. 1x is as shipped; "
               "up to 3x, past the game's highest Controller Sensitivity, for a pad that turns as fast as a mouse. "
               "It scales turning left, right, up and down at every stick position, and the game's own Controller "
               "Sensitivity option still applies on top. Ctrl-click to type a value.",
               {}});
  s.back().slider_min = 1, s.back().slider_max = 3, s.back().slider_linear = true, s.back().slider_format = "%.2fx";
  s.push_back({"vibration", "", "Vibration", "Turn off to stop the controller rumbling.", {}});
  s.push_back({"user_signin_live", "", "LAN co-op",
               "Play the story with another computer on your network. One player starts a story; the other picks "
               "Play Game, then Join COOP, and chooses it from the list. Both computers need this on. On Windows, "
               "allow Skullburglar through the firewall when asked, or games won't be found.",
               {}});
  s.back().section = "Co-op";
  s.push_back({"split_screen", "", "Split-screen co-op (experimental)",
               "One window, split top and bottom. During a story, press START on a second controller to join as "
               "player 2; player 2 leaves with Quit in their pause menu. The two-window option below overrides it.",
               {}});
  s.push_back({"coop", "", "Co-op on this PC, two windows (experimental)",
               "Opens a second window for player 2. Player 1 starts a story; player 2 picks Join COOP and the game "
               "from the list.",
               {}});
  s.push_back({"coop_pads", "", "Co-op controllers", "Controllers in the order they were connected.",
               {{"1,2", "P1: pad 1, P2: pad 2"}, {"2,1", "P1: pad 2, P2: pad 1"}}});
  s.push_back({"kbm", "", "Keyboard and mouse", "Play with keyboard and mouse. A controller always works too.", {}});
  s.back().section = "Keyboard and mouse";
  s.push_back({"kbm_sensitivity", "", "Mouse sensitivity",
               "How fast the camera turns, when not aiming. 1 is the game's own speed. Ctrl-click to type a value.",
               {}});
  s.back().slider_min = 0.1, s.back().slider_max = 4;
  s.push_back({"kbm_aim_sensitivity", "", "Aiming sensitivity",
               "How fast the camera turns while you hold Aim (the game already slows it as it zooms in). Ctrl-click "
               "to type a value.",
               {}});
  s.back().slider_min = 0.1, s.back().slider_max = 4;
  s.push_back({"kbm_invert_y", "", "Invert mouse up/down", "", {}});
  s.push_back({"kbm_noise_gate", "", "Mouse noise gate (pixels)",
               "0 = off. If the camera drifts slowly while your mouse or gyro is still, try 1 or 2: motion shorter "
               "than this is held back until it adds up. Ctrl-click to type a value.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 4;
  s.push_back({"kbm_prompts", "", "Keyboard and mouse button prompts",
               "Show keys instead of controller buttons in prompts while you play with keyboard and mouse.", {}});
  static const struct {
    const char* cvar;
    const char* label;
  } kBindings[] = {
      {"kbm_move_forward", "Move forward"}, {"kbm_move_back", "Move back"}, {"kbm_move_left", "Move left"},
      {"kbm_move_right", "Move right"}, {"kbm_right_trigger", "Fire"}, {"kbm_left_trigger", "Aim"},
      {"kbm_accelerate", "Accelerate (driving)"}, {"kbm_brake", "Brake / reverse (driving)"},
      {"kbm_a", "Cover / use / confirm"}, {"kbm_b", "Special move / counter-kill / back"},
      {"kbm_x", "Reload / pick up weapon"}, {"kbm_y", "Gangsta Fury / skip"},
      {"kbm_right_shoulder", "Throw grenade"}, {"kbm_left_shoulder", "Next grenade"},
      {"kbm_dpad_up", "Weapon slot up"}, {"kbm_dpad_right", "Weapon slot right"},
      {"kbm_dpad_down", "Weapon slot down"}, {"kbm_dpad_left", "Weapon slot left"},
      {"kbm_left_thumb", "Taunt"}, {"kbm_right_thumb", "Right stick click"}, {"kbm_back", "Objectives"},
      {"kbm_start", "Pause"}, {"kbm_confirm", "Menu confirm / Press START"},
  };
  for (const auto& b : kBindings) {
    s.push_back({b.cvar, "", b.label, "", {}});
    s.back().binding = true;
  }
  return s;
}

std::vector<std::string>& AppDefaults() {
  static std::vector<std::string> names;
  return names;
}

std::vector<std::pair<std::string, std::string>>& AppDefaultValues() {
  static std::vector<std::pair<std::string, std::string>> values;
  return values;
}

// Text as the window holds it: the renderer is "new" unless pre1, and a double reads back as "1.000000", so show it as
// the choice it matches.
std::string Normalize(const Setting& s, const rex::cvar::FlagEntry* info, std::string text) {
  if (s.cvar == "renderer" && text != "pre1") return "new";
  if (info && info->type == rex::cvar::FlagType::Double) {
    for (const auto& choice : s.choices) {
      if (SameValue(choice.value, text)) return choice.value;
    }
  }
  return text;
}

bool IsLocked(const std::string& cvar) {
  if (std::find(AppDefaults().begin(), AppDefaults().end(), cvar) != AppDefaults().end()) {
    return false;
  }
  auto src = rex::cvar::GetFlagSource(cvar);
  return src == rex::cvar::Source::kEnvironment || src == rex::cvar::Source::kCommandLine;
}

}  // namespace

void MarkAppDefault(const std::string& cvar) { AppDefaults().push_back(cvar); }

void RegisterAppDefault(const std::string& cvar, const std::string& value) {
  AppDefaultValues().emplace_back(cvar, value);
}

std::vector<Setting> LoadSettings() {
  auto settings = Catalog();
  // Settings whose cvar the GPU plugin didn't register aren't shown; the renderer choice needs a build with n1.
  std::erase_if(settings, [](const Setting& s) {
    return !rex::cvar::GetFlagInfo(s.cvar) || (s.cvar == "check_updates" && !UpdateCheckAvailable()) ||
           (s.cvar == "renderer" && !bots::kHasN1);
  });
  for (auto& s : settings) {
    const auto* info = rex::cvar::GetFlagInfo(s.cvar);
    s.value = Normalize(s, info, rex::cvar::GetFlagByName(s.cvar));
    s.locked = IsLocked(s.cvar);
    s.default_value = info->default_value;
    for (const auto& [cvar, value] : AppDefaultValues()) {
      if (cvar == s.cvar) s.default_value = value;
    }
    s.default_value = Normalize(s, info, s.default_value);
  }
  return settings;
}

namespace {

// Opens the file by its path (the wide name on Windows), not toml::parse_file's narrow name, which can't hold every
// folder name. A file that can't be opened reads as empty; the path in parse errors is UTF-8.
toml::table ParseToml(const fs::path& config_path) {
  std::ifstream in(config_path, std::ios::binary);
  if (!in) return {};
  return toml::parse(in, bots::PathUtf8(config_path));
}

toml::table ReadConfig(const fs::path& config_path) {
  std::error_code ec;
  if (fs::exists(config_path, ec)) {
    try {
      return ParseToml(config_path);
    } catch (const toml::parse_error& err) {
      REXLOG_WARN("setup: {} unreadable ({}), rewriting it", bots::PathUtf8(config_path), err.what());
    }
  }
  return {};
}

std::string WriteConfig(const toml::table& table, const fs::path& config_path) {
  std::error_code ec;
  fs::create_directories(config_path.parent_path(), ec);
  std::ofstream out(config_path, std::ios::trunc);
  if (!out) {
    // The reason matters: a path that is too long reads "No such file or directory" and looks like a missing folder.
    const std::string why = ec ? ec.message() : std::error_code(errno, std::generic_category()).message();
    return "Can't write " + bots::PlainPathUtf8(config_path) + " (" + why + ")";
  }
  out << "# bots settings (also editable in the launcher, which opens on start)\n" << table << "\n";
  return {};
}

}  // namespace

std::string SaveSettings(const std::vector<Setting>& settings, const fs::path& config_path) {
  toml::table table = ReadConfig(config_path);
  for (const auto& s : settings) {
    if (s.locked) continue;
    for (const std::string& name : {s.cvar, s.also_cvar}) {
      if (name.empty()) continue;
      if (!rex::cvar::SetFlagByName(name, s.value)) {
        REXLOG_WARN("setup: can't set {} = {}", name, s.value);
      }
      if (!ShouldWriteKey(table, name, s.value, s.default_value)) continue;
      const auto* info = rex::cvar::GetFlagInfo(name);
      auto type = info ? info->type : rex::cvar::FlagType::String;
      if (type == rex::cvar::FlagType::Boolean) {
        table.insert_or_assign(name, s.value == "true");
      } else if (type == rex::cvar::FlagType::Double) {
        table.insert_or_assign(name, std::strtod(s.value.c_str(), nullptr));
      } else if (type == rex::cvar::FlagType::String) {
        table.insert_or_assign(name, s.value);
      } else {
        table.insert_or_assign(name, int64_t(std::stoll(s.value)));
      }
    }
  }
  // Older launchers wrote the renderer as gpu_plugin; it is ignored now (bots_app.h), so don't leave it behind.
  if (const auto* old = table.get("gpu_plugin"); old && old->is_string()) table.erase("gpu_plugin");
  std::string err = WriteConfig(table, config_path);
  if (err.empty()) REXLOG_INFO("setup: settings saved to {}", bots::PathUtf8(config_path));
  return err;
}

std::string ResetSettings(std::vector<Setting>& settings, const fs::path& config_path, int* changed) {
  toml::table table = ReadConfig(config_path);
  int count = 0;
  for (auto& s : settings) {
    if (s.locked || ResetKeeps(s.cvar)) continue;
    if (!SameValue(s.value, s.default_value)) ++count;
    s.value = s.default_value;
    for (const std::string& name : {s.cvar, s.also_cvar}) {
      if (name.empty()) continue;
      if (!rex::cvar::SetFlagByName(name, s.value)) REXLOG_WARN("setup: can't set {} = {}", name, s.value);
      table.erase(name);
    }
  }
  if (changed) *changed = count;
  std::string err = WriteConfig(table, config_path);
  if (err.empty()) REXLOG_INFO("setup: {} settings reset to this version's defaults in {}", count, bots::PathUtf8(config_path));
  return err;
}

bool ConfigHasKey(const fs::path& config_path, const std::string& key) {
  std::error_code ec;
  if (!fs::exists(config_path, ec)) return false;
  try {
    return ParseToml(config_path).contains(key);
  } catch (const toml::parse_error&) {
    return false;
  }
}

std::string ConfigString(const fs::path& config_path, const std::string& key) {
  std::error_code ec;
  if (!fs::exists(config_path, ec)) return {};
  try {
    return ParseToml(config_path)[key].value_or(std::string());
  } catch (const toml::parse_error&) {
    return {};
  }
}

std::string SetConfigString(const fs::path& config_path, const std::string& key, const std::string& value) {
  toml::table table = ReadConfig(config_path);
  if (value.empty()) {
    table.erase(key);
  } else {
    table.insert_or_assign(key, value);
  }
  return WriteConfig(table, config_path);
}

bool ConfigHasPath(const fs::path& config_path, const std::string& key) {
  return ConfigHasKey(config_path, key) || ConfigHasKey(config_path, key + "_hex");
}

fs::path ConfigPath(const fs::path& config_path, const std::string& key) {
  const std::string text = ConfigString(config_path, key);
  if (!text.empty()) return bots::PathFromUtf8(text);
  bool ok = false;
  const std::string bytes = bots::HexDecode(ConfigString(config_path, key + "_hex"), &ok);
  return ok && !bytes.empty() ? bots::PathFromUtf8(bytes) : fs::path();
}

std::string SetConfigPath(const fs::path& config_path, const std::string& key, const fs::path& path) {
  const std::string text = bots::PlainPathUtf8(path);  // never the \\?\ form (path_utf8.h): bots.toml stays readable
  toml::table table = ReadConfig(config_path);
  table.erase(key);
  table.erase(key + "_hex");
  if (bots::IsValidUtf8(text)) {
    if (!text.empty()) table.insert_or_assign(key, text);
  } else {
    table.insert_or_assign(key + "_hex", bots::HexEncode(text));
  }
  return WriteConfig(table, config_path);
}

}  // namespace bots::setup
