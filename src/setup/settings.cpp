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
REXCVAR_DEFINE_STRING(msaa, "off", "bots/Display", "New renderer multisampling: off, 2x, 4x, 2x-sgssaa or 4x-sgssaa");
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
REXCVAR_DEFINE_BOOL(ambient_occlusion, false, "bots/Display", "Soft contact shading with the new renderer");
REXCVAR_DEFINE_DOUBLE(ambient_occlusion_strength, 3.0, "bots/Display", "Ambient occlusion strength");

namespace bots::setup {
namespace fs = std::filesystem;

namespace {

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
  auto settings = SettingsCatalog();
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

// Writes bots.toml atomically: the text goes to a temp file beside it, then replaces the file in one rename, so a crash,
// a full disk or a quit in the middle never leaves half a file. A file marked read-only is refused (the rename would
// replace it silently). Returns an error message, empty on success.
std::string WriteConfig(const toml::table& table, const fs::path& config_path) {
  std::error_code ec;
  fs::create_directories(config_path.parent_path(), ec);
  const auto why_of = [&](const std::error_code& e) {
    return e ? e.message() : std::error_code(errno, std::generic_category()).message();
  };
  if (fs::exists(config_path, ec)) {
    const auto perms = fs::status(config_path, ec).permissions();
    if (!ec && (perms & fs::perms::owner_write) == fs::perms::none)
      return "Can't write " + bots::PlainPathUtf8(config_path) + " (the file is read-only)";
  }
  fs::path temp = config_path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      // The reason matters: a path that is too long reads "No such file or directory" and looks like a missing folder.
      return "Can't write " + bots::PlainPathUtf8(config_path) + " (" + why_of(ec) + ")";
    }
    out << "# bots settings (also editable in the launcher, which opens on start)\n" << table << "\n";
    out.flush();
    if (!out) {
      const std::string why = why_of({});
      out.close();
      fs::remove(temp, ec);
      return "Can't write " + bots::PlainPathUtf8(config_path) + " (" + why + ")";
    }
  }
  fs::rename(temp, config_path, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::remove(temp, ec);
    return "Can't write " + bots::PlainPathUtf8(config_path) + " (" + why + ")";
  }
  return {};
}

}  // namespace

std::string SaveSettings(const std::vector<Setting>& settings, const fs::path& config_path) {
  return SaveChangedSettings(settings, nullptr, config_path);
}

std::string SaveChangedSettings(const std::vector<Setting>& settings, const std::vector<Setting>* saved,
                                const fs::path& config_path) {
  toml::table table = ReadConfig(config_path);
  int written = 0;
  std::string names;  // for the log
  for (const auto& s : settings) {
    if (s.locked) continue;
    if (saved) {  // only what differs from what was last written
      const auto was = std::find_if(saved->begin(), saved->end(), [&](const Setting& o) { return o.cvar == s.cvar; });
      if (was != saved->end() && SameValue(was->value, s.value)) continue;
    }
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
      ++written;
      if (saved && written <= 6) names += (names.empty() ? "" : ", ") + name + "=" + s.value;
    }
  }
  // Older launchers wrote the renderer as gpu_plugin; it is ignored now (bots_app.h), so don't leave it behind.
  if (const auto* old = table.get("gpu_plugin"); old && old->is_string()) table.erase("gpu_plugin");
  if (saved && written == 0) return {};  // nothing in the file changes (a value back at its default with no key)
  std::string err = WriteConfig(table, config_path);
  if (err.empty())
    REXLOG_INFO("setup: settings saved to {} ({} key(s){}{})", bots::PathUtf8(config_path), written, names.empty() ? "" : ": ", names);
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
