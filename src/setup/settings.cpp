// See settings.h.
#include "../path_utf8.h"
#include "settings.h"

#include "../renderer_choice.h"
#include "update_check.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <system_error>

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
REXCVAR_DEFINE_STRING(renderer, bots::kDefaultRenderer, "bots",
                      "Which renderer draws the game: new (the default; on tvOS pre1 is) or pre1, the pre-1.0 renderer "
                      "(deprecated). Only pre1 selects the old one; any other value is the new renderer");
REXCVAR_DEFINE_STRING(render_scale, "auto", "bots",
                      "Render resolution as a multiple of 720p (auto, 3, 2, 1.5, 1.25, 1, 0.75, "
                      "0.667, 0.5); auto is 2/3 on Intel GPUs, else 1");
REXCVAR_DEFINE_BOOL(present_vsync, true, "bots",
                    "Wait for the display's refresh to show each frame (Vulkan presentation): no tearing. Off presents "
                    "immediately, with the lowest latency, and can tear. (Not `vsync`, the emulated console's vblank.)");

REXCVAR_DEFINE_BOOL(fps_counter, false, "bots",
                    "Show the frame rate, frame time and renderer in a corner of the screen (fps_counter.h)");

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
  s.push_back({"render_scale", "", "Render resolution",
               "Higher is sharper and costs GPU time; lower is faster on weak GPUs. Automatic is 720p, or 67% on "
               "Intel graphics.",
               {{"auto", "Automatic"}, {"3", "4K  (2160p)"}, {"2", "1440p"}, {"1.5", "1080p"},
                {"1.25", "900p"}, {"1", "720p  (original)"}, {"0.75", "75%"}, {"0.667", "67%"}, {"0.5", "50%"}}});
  s.push_back({"aspect_ratio", "", "Aspect ratio",
               "Wider than 16:9 fills an ultrawide screen and shows more at the sides; the HUD and menus stretch. "
               "Automatic matches the screen in fullscreen, or the window.",
               {{"16:9", "16:9 (original)"}, {"auto", "Automatic"}, {"21:9", "21:9"}, {"32:9", "32:9"}}});
  s.push_back({"motion_blur", "", "Motion blur",
               "The smear when the camera turns or things move fast. Off keeps every other effect.",
               {}});
  s.push_back({"screen_shake", "", "Screen shake",
               "How much the camera shakes from explosions, gunfire and hits, as a share of the game's own. 100% is "
               "as shipped; 0% turns it off.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 100, s.back().slider_linear = true, s.back().slider_format = "%.0f%%";
  s.push_back({"framerate_limit", "", "Frame rate limit", "",
               {{"30", "30 fps"}, {"60", "60 fps (recommended)"}, {"0", "Unlimited"}}});
  s.push_back({"present_vsync", "", "VSync", "Off has the lowest latency but can tear.", {}});
  s.push_back({"fps_counter", "", "Show FPS counter",
               "A small readout in the top-left corner: frames per second, milliseconds per frame, and which renderer "
               "is drawing. Handy for bug reports. Takes effect the next time the game starts.",
               {}});
  s.push_back({"check_updates", "", "Check for new versions",
               "When the launcher opens, look online for a newer Skullburglar and offer its download page.", {}});
  s.push_back({"pad_prompts", "", "Controller button prompts",
               "Which buttons the game shows. Automatic matches the controller you used last.",
               {{"auto", "Automatic"}, {"xbox", "Xbox"}, {"ps4", "PlayStation 4"}, {"ps5", "PlayStation 5"}}});
  s.back().section = "Controller";
  s.push_back({"aim_assist", "", "Aim assist",
               "The game slows the crosshair and pulls it toward enemies near it while you move (even through walls, "
               "as on the Xbox 360). Turn off for raw aiming. Applies to the mouse too.",
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

bool IsLocked(const std::string& cvar) {
  if (std::find(AppDefaults().begin(), AppDefaults().end(), cvar) != AppDefaults().end()) {
    return false;
  }
  auto src = rex::cvar::GetFlagSource(cvar);
  return src == rex::cvar::Source::kEnvironment || src == rex::cvar::Source::kCommandLine;
}

}  // namespace

void MarkAppDefault(const std::string& cvar) { AppDefaults().push_back(cvar); }

std::vector<Setting> LoadSettings() {
  auto settings = Catalog();
  // Settings whose cvar the GPU plugin didn't register aren't shown; the renderer choice needs a build with n1.
  std::erase_if(settings, [](const Setting& s) {
    return !rex::cvar::GetFlagInfo(s.cvar) || (s.cvar == "check_updates" && !UpdateCheckAvailable()) ||
           (s.cvar == "renderer" && !bots::kHasN1);
  });
  for (auto& s : settings) {
    s.value = rex::cvar::GetFlagByName(s.cvar);
    if (s.cvar == "renderer" && s.value != "pre1") s.value = "new";  // anything but pre1 is the new renderer
    s.locked = IsLocked(s.cvar);
    const auto* info = rex::cvar::GetFlagInfo(s.cvar);
    s.default_value = info->default_value;
    // A double reads back as "1.000000": show it as the matching choice.
    if (info->type == rex::cvar::FlagType::Double) {
      for (const auto& choice : s.choices) {
        if (std::abs(std::strtod(choice.value.c_str(), nullptr) - std::strtod(s.value.c_str(), nullptr)) < 1e-6) {
          s.value = choice.value;
        }
      }
    }
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
  if (!out) return "Can't write " + bots::PathUtf8(config_path);
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
  const std::string text = bots::PathUtf8(path);
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
