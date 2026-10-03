// See settings.h.
#include "settings.h"

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
REXCVAR_DEFINE_BOOL(launcher, false, "bots", "Show the setup / launcher window before starting the game");
REXCVAR_DEFINE_STRING(render_scale, "auto", "bots",
                      "Native renderer: render resolution as a fraction of 720p (auto, 1, 0.75, 0.667, 0.5); "
                      "auto is 2/3 on Intel GPUs, else 1");

namespace bots::setup {
namespace fs = std::filesystem;

namespace {

std::vector<Setting> Catalog() {
  std::vector<Setting> s;
  s.push_back({"fullscreen", "", "Fullscreen", "F11 toggles it in game.", {}});
  s.push_back({"intro_movies", "", "Play intro movies", "The logo movies before the title screen.", {}});
  s.push_back({"gpu_plugin", "", "Renderer", "Takes effect the next time the game starts.",
               {{"native", "Native (Vulkan, recommended)"}, {"xenos", "Emulated Xbox 360 GPU"}}});
  s.push_back({"render_scale", "", "Render resolution (native)",
               "Lower is faster on weak GPUs. Automatic is 67% on Intel graphics, else 100%.",
               {{"auto", "Automatic"}, {"1", "100%  (720p, original)"},
                {"0.75", "75%"}, {"0.667", "67%"}, {"0.5", "50%"}}});
  s.push_back({"draw_resolution_scale_x", "draw_resolution_scale_y", "Render resolution (emulated)",
               "Higher is sharper and costs GPU time. 3x needs a fast GPU.",
               {{"1", "1x  (720p, original)"}, {"2", "2x  (1440p)"}, {"3", "3x  (2160p / 4K)"}}});
  s.back().emulated_only = true;
  s.push_back({"swap_post_effect", "", "Anti-aliasing", "",
               {{"none", "Off"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA (extreme)"}}});
  s.back().emulated_only = true;
  s.push_back({"anisotropic_override", "", "Texture filtering", "Anisotropic filtering for floors and walls.",
               {{"-1", "Game default"}, {"0", "Off"}, {"1", "1x"}, {"2", "2x"}, {"3", "4x"},
                {"4", "8x"}, {"5", "16x"}}});
  s.back().emulated_only = true;
  s.push_back({"framerate_limit", "", "Frame rate limit", "",
               {{"30", "30 fps"}, {"60", "60 fps (recommended)"}, {"0", "Unlimited"}}});
  s.push_back({"kbm", "", "Keyboard and mouse", "Play with keyboard and mouse. A controller always works too.", {}});
  s.back().section = "Keyboard and mouse";
  s.push_back({"kbm_sensitivity", "", "Mouse sensitivity", "",
               {{"0.25", "0.25"}, {"0.5", "0.5"}, {"0.75", "0.75"}, {"1", "1 (default)"}, {"1.5", "1.5"},
                {"2", "2"}, {"3", "3"}, {"4", "4"}}});
  s.push_back({"kbm_invert_y", "", "Invert mouse up/down", "", {}});
  s.push_back({"kbm_prompts", "", "Keyboard and mouse button prompts",
               "Show keys instead of controller buttons in prompts while you play with keyboard and mouse.", {}});
  static const struct {
    const char* cvar;
    const char* label;
  } kBindings[] = {
      {"kbm_move_forward", "Move forward"}, {"kbm_move_back", "Move back"}, {"kbm_move_left", "Move left"},
      {"kbm_move_right", "Move right"}, {"kbm_right_trigger", "Fire"}, {"kbm_left_trigger", "Aim"},
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
#if defined(_WIN32)
  s.push_back({"d3d12_present_sync_interval", "", "VSync", "Off has the lowest latency but can tear.",
               {{"0", "Off"}, {"1", "On"}}});
  s.back().emulated_only = true;
#endif
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
  // Settings of the renderer not in use, or whose cvar its plugin didn't register, aren't shown.
  const bool native = rex::cvar::GetFlagByName("gpu_plugin") == "native";
  std::erase_if(settings, [native](const Setting& s) {
    return (native && s.emulated_only) || !rex::cvar::GetFlagInfo(s.cvar);
  });
  for (auto& s : settings) {
    s.value = rex::cvar::GetFlagByName(s.cvar);
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

std::string SaveSettings(const std::vector<Setting>& settings, const fs::path& config_path) {
  toml::table table;
  std::error_code ec;
  if (fs::exists(config_path, ec)) {
    try {
      table = toml::parse_file(config_path.string());
    } catch (const toml::parse_error& err) {
      REXLOG_WARN("setup: {} unreadable ({}), rewriting it", config_path.string(), err.what());
    }
  }
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
  fs::create_directories(config_path.parent_path(), ec);
  std::ofstream out(config_path, std::ios::trunc);
  if (!out) return "Can't write " + config_path.string();
  out << "# bots settings (also editable in the launcher: bots --launcher)\n" << table << "\n";
  REXLOG_INFO("setup: settings saved to {}", config_path.string());
  return {};
}

bool ConfigHasKey(const fs::path& config_path, const std::string& key) {
  std::error_code ec;
  if (!fs::exists(config_path, ec)) return false;
  try {
    return toml::parse_file(config_path.string()).contains(key);
  } catch (const toml::parse_error&) {
    return false;
  }
}

std::string ConfigString(const fs::path& config_path, const std::string& key) {
  std::error_code ec;
  if (!fs::exists(config_path, ec)) return {};
  try {
    return toml::parse_file(config_path.string())[key].value_or(std::string());
  } catch (const toml::parse_error&) {
    return {};
  }
}

}  // namespace bots::setup
