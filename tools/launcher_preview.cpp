// The launcher's menu without the game (B-387): draws src/setup/launcher_menu.h with the real settings list and the
// fonts in assets/fonts, at any size and display scaling, drives it with a script of key, controller and mouse events
// and writes PNG shots (src/setup/launcher_shot.h has the script steps). Built and run by tools/launcher_preview.sh;
// needs only a C++ compiler and Dear ImGui's sources.
//
//   launcher_preview [--out DIR] [--sizes LIST] [--script STEPS|@FILE] [--state NAME] [--check] [--fonts DIR]
//
// --state picks what the launcher knows: installed (default), first-run, installing, update, updating, locked
// (a setting set by a launch option), kbm-off. --check exits 1 if any shot's layout check saw text cut off, prompts
// left out or the focused row out of view, or an expect: step failed. The values are the settings' built-in defaults
// (a copy of the cvar definitions; the game reads the real ones).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "setup/launcher_menu.h"
#include "setup/launcher_shot.h"
#include "setup/settings.h"

using namespace bots::setup;

namespace {

// The cvars' built-in values (REXCVAR_DEFINE_* in src/ and the SDK), for a realistic picture.
const std::map<std::string, std::string> kDefaults = {
    {"fullscreen", "true"}, {"intro_movies", "false"}, {"achievement_popups", "false"}, {"renderer", "new"},
    {"render_scale", "auto"}, {"aspect_ratio", "16:9"}, {"fov_offset", "0.00"}, {"vulkan_hdr", "false"},
    {"vulkan_hdr_paper_white", "203.00"}, {"vulkan_hdr_peak", "1000.00"}, {"vulkan_hdr_ui_white", "203.00"},
    {"vulkan_hdr_exposure", "0.00"}, {"vulkan_hdr_middle_gray", "0.180"}, {"vulkan_hdr_contrast", "1.00"},
    {"vulkan_hdr_highlights", "1.00"}, {"vulkan_hdr_shadows", "1.00"}, {"vulkan_hdr_saturation", "1.00"},
    {"motion_blur", "true"}, {"screen_shake", "100.00"}, {"movie_volume", "100.00"}, {"framerate_limit", "60"},
    {"present_vsync", "true"}, {"fps_counter", "false"}, {"check_updates", "true"}, {"diagnostic_logging", "false"},
    {"pad_prompts", "auto"}, {"aim_assist", "true"}, {"right_stick_deadzone", "20.00"},
    {"stick_acceleration", "true"}, {"stick_response", "original"}, {"stick_look_speed", "1.00"},
    {"vibration", "true"}, {"user_signin_live", "true"}, {"split_screen", "false"}, {"coop", "false"},
    {"coop_pads", "1,2"}, {"kbm", "true"}, {"kbm_sensitivity", "1.00"}, {"kbm_aim_sensitivity", "1.00"},
    {"kbm_invert_y", "false"}, {"kbm_noise_gate", "0.00"}, {"kbm_prompts", "true"},
    {"kbm_move_forward", "W"}, {"kbm_move_back", "S"}, {"kbm_move_left", "A"}, {"kbm_move_right", "D"},
    {"kbm_right_trigger", "LMB"}, {"kbm_left_trigger", "RMB,Shift"}, {"kbm_accelerate", "W"}, {"kbm_brake", "S"},
    {"kbm_a", "Space"}, {"kbm_b", "E,F,Backspace"}, {"kbm_x", "R"}, {"kbm_y", "Q"}, {"kbm_right_shoulder", "G,MMB"},
    {"kbm_left_shoulder", "T"}, {"kbm_dpad_up", "1,Up,WheelUp"}, {"kbm_dpad_down", "3,Down,WheelDown"},
    {"kbm_dpad_left", "4,Left"}, {"kbm_dpad_right", "2,Right"}, {"kbm_left_thumb", "C"}, {"kbm_right_thumb", "V"},
    {"kbm_back", "Tab"}, {"kbm_start", "Escape"}, {"kbm_confirm", "Return"},
};

std::vector<char> ReadFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(f), {});
}

// Every page at every size, scrolled to its end, plus the overlays: what --check looks at by default.
const char* kDefaultScript =
    "shot:main;page:graphics;shot:graphics;focus:fps_counter;shot:graphics-end;page:hdr;focus:vulkan_hdr_saturation;"
    "shot:hdr-end;page:game;shot:game;page:controls;shot:controls;focus:kbm_prompts;shot:controls-end;page:bindings;"
    "shot:bindings;focus:kbm_confirm;shot:bindings-end;page:coop;shot:coop;page:files;shot:files;page:updates;"
    "shot:updates;page:about;shot:about;reset-confirm;shot:reset-confirm";

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path out = "launcher-shots";
  std::string sizes = "1280x800,1280x720,1920x1080,2560x1440,3840x2160,3840x2160@1.5,3840x2160@2,3440x1440,1024x768";
  std::string script = kDefaultScript;
  std::string state = "installed";
  std::filesystem::path fonts_dir = std::filesystem::path(__FILE__).parent_path().parent_path() / "assets" / "fonts";
  bool check = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--out") out = next();
    else if (a == "--sizes") sizes = next();
    else if (a == "--script") script = next();
    else if (a == "--state") state = next();
    else if (a == "--fonts") fonts_dir = next();
    else if (a == "--check") check = true;
    else {
      std::fprintf(stderr, "usage: launcher_preview [--out DIR] [--sizes LIST] [--script STEPS] [--state NAME] "
                           "[--fonts DIR] [--check]\n");
      return 2;
    }
  }

  const auto inter_r = ReadFile(fonts_dir / "Inter-Regular.ttf");
  const auto inter_sb = ReadFile(fonts_dir / "Inter-SemiBold.ttf");
  const auto oswald = ReadFile(fonts_dir / "Oswald-SemiBold.ttf");
  if (inter_r.empty() || inter_sb.empty() || oswald.empty()) {
    std::fprintf(stderr, "launcher_preview: fonts not found in %s\n", fonts_dir.string().c_str());
    return 2;
  }

  const std::vector<Setting> pristine = [&] {
    auto list = SettingsCatalog();
    for (auto& s : list) {
      const auto it = kDefaults.find(s.cvar);
      s.default_value = it != kDefaults.end() ? it->second : (s.choices.empty() ? "false" : s.choices[0].value);
      s.value = s.default_value;
      if (state == "locked" && s.cvar == "render_scale") s.locked = true, s.value = "2";
      if (state == "kbm-off" && s.cvar == "kbm") s.value = "false";
    }
    return list;
  }();
  std::vector<Setting> settings = pristine;

  char iso[1024] = "";
  LauncherStatus st;
  st.version = "1.13.0";
  st.installed = true;
  st.game_dir = "C:\\Users\\Player\\AppData\\Local\\bots-recomp\\game";
  st.config_path = "C:\\Users\\Player\\AppData\\Local\\bots-recomp\\bots.toml";
  st.log_folder = "C:\\Users\\Player\\AppData\\Local\\bots-recomp\\logs";
  st.iso_path = iso;
  st.iso_path_size = sizeof(iso);
  st.update_available_feature = true;
  st.firewall = 2;  // allowed
  if (state == "first-run") st.installed = false;
  if (state == "installing") {
    st.installed = false, st.extracting = true, st.extract_fraction = 0.42f;
    st.extract_progress = "2.9 / 6.8 GB";
    st.extract_file = "File 311 of 737: C1Game/CookedXenon/Chapter_03.xxx";
    std::snprintf(iso, sizeof(iso), "D:\\Games\\Blood on the Sand.iso");
  }
  if (state == "update" || state == "updating") {
    st.update_newer = true, st.update_latest = "1.14.0", st.update_can_install = true, st.update_can_download = true;
  }
  if (state == "updating") st.update_busy = true, st.update_stage = 0, st.update_done_mb = 61.2, st.update_total_mb = 164.0;

  ShotHost host;
  host.fonts.inter_regular = inter_r.data(), host.fonts.inter_regular_size = inter_r.size();
  host.fonts.inter_semibold = inter_sb.data(), host.fonts.inter_semibold_size = inter_sb.size();
  host.fonts.oswald_semibold = oswald.data(), host.fonts.oswald_semibold_size = oswald.size();
  host.frame = [&](LauncherMenu& menu, const LauncherFonts& fonts, float dpi, PadKind pad) {
    st.dpi_scale = dpi;
    st.pad = pad;
    const LauncherAction a = menu.Draw(settings, st, fonts);
    if (a == LauncherAction::kResetAll) {
      for (auto& s : settings)
        if (!s.locked && s.cvar != "check_updates") s.value = s.default_value;
      st.reset_note = "Settings reset to this version's defaults.";
    }
    if (a == LauncherAction::kShowReinstall) st.show_reinstall = true;
    if (a == LauncherAction::kHideReinstall) st.show_reinstall = false;
    return a;
  };
  host.value_of = [&](const std::string& cvar) {
    for (const auto& s : settings)
      if (s.cvar == cvar) return s.value;
    return std::string("(missing)");
  };
  host.reset = [&] {
    settings = pristine;
    st.reset_note.clear();
  };
  int problems = 0;
  host.log = [&](const std::string& line) {
    std::printf("%s\n", line.c_str());
    if (line.rfind("shot ", 0) == 0 &&
        (line.find("cut-off text 0,") == std::string::npos || line.find("prompts left out 0,") == std::string::npos ||
         line.find("OUT OF VIEW") != std::string::npos))
      ++problems;
  };
  std::error_code ec;
  std::filesystem::create_directories(out, ec);
  std::filesystem::remove(out / "shots.txt", ec);
  ImGui::CreateContext();  // RunShotScript makes its own per size and puts this one back
  const int failures = RunShotScript(host, out, ParseShotSizes(sizes), ParseShotScript(script));
  std::printf("launcher_preview: %d layout problem(s), %d failure(s)\n", problems, failures);
  return (failures || (check && problems)) ? 1 : 0;
}
