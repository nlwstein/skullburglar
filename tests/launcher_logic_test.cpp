// Plain-assert tests for src/setup/launcher_logic.h (the launcher menu's rules) and LauncherMenuScale (ui_scale.h).
// Run: c++ -std=c++20 -I src tests/launcher_logic_test.cpp src/setup/settings_catalog.cpp -o /tmp/t && /tmp/t
#include "setup/launcher_logic.h"
#include "setup/ui_scale.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>

using namespace bots::setup;

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static Setting Slider(const char* cvar, double lo, double hi, bool linear) {
  Setting s;
  s.cvar = cvar, s.slider_min = lo, s.slider_max = hi, s.slider_linear = linear;
  return s;
}

int main() {
  // Every catalog setting names its page (not "none", not the main menu or the game-files page) and no page is guessed
  // from a section or a name: a setting added without a page fails here. Every page that takes settings has some.
  const auto catalog = SettingsCatalog();
  std::set<LauncherPage> used;
  std::set<std::string> cvars;
  for (const auto& s : catalog) {
    assert(PageTakesSettings(s.page) && "a catalog setting names no launcher page");
    assert(cvars.insert(s.cvar).second && "duplicate cvar in the catalog");
    used.insert(s.page);
    assert((s.page == LauncherPage::kBindings) == s.binding);
    assert(s.group.empty() || s.page != LauncherPage::kBindings);
  }
  for (LauncherPage p : {LauncherPage::kGraphics, LauncherPage::kHdr, LauncherPage::kGame, LauncherPage::kControls,
                         LauncherPage::kCoop, LauncherPage::kUpdates, LauncherPage::kAbout, LauncherPage::kBindings})
    assert(used.count(p));
  auto page_of = [&](const char* cvar) {
    for (const auto& s : catalog)
      if (s.cvar == cvar) return s.page;
    assert(false && "not in the catalog");
    return LauncherPage::kMain;
  };
  auto group_of = [&](const char* cvar) {
    for (const auto& s : catalog)
      if (s.cvar == cvar) return s.group;
    assert(false && "not in the catalog");
    return std::string();
  };
  assert(page_of("fullscreen") == LauncherPage::kGraphics);
  assert(page_of("fov_offset") == LauncherPage::kGraphics);
  assert(page_of("anti_aliasing") == LauncherPage::kGraphics);
  assert(page_of("sharpening") == LauncherPage::kGraphics);
  assert(page_of("ambient_occlusion") == LauncherPage::kGraphics && page_of("ambient_occlusion_strength") == LauncherPage::kGraphics);
  assert(page_of("kbm_noise_gate_strict") == LauncherPage::kControls);
  assert(page_of("pre1_occlusion") == LauncherPage::kGraphics);
  assert(page_of("aim_toggle") == LauncherPage::kControls);
  assert(page_of("vulkan_hdr_peak") == LauncherPage::kHdr);
  assert(page_of("movie_volume") == LauncherPage::kGame);
  assert(page_of("kbm_sensitivity") == LauncherPage::kControls);
  assert(page_of("pad_prompts") == LauncherPage::kControls);
  assert(page_of("coop_pads") == LauncherPage::kCoop);
  assert(page_of("check_updates") == LauncherPage::kUpdates);
  assert(page_of("diagnostic_logging") == LauncherPage::kAbout);
  assert(page_of("kbm_move_forward") == LauncherPage::kBindings);
  // Groups: the movie volume sits with the intro movies, apart from the effects; frame rate, VSync and the FPS counter
  // are together; aim assist and toggle aim (they work with the mouse too) are not under "Controller".
  assert(group_of("movie_volume") == "Movies and audio" && group_of("intro_movies") == "Movies and audio");
  assert(group_of("motion_blur") == "Effects" && group_of("screen_shake") == "Effects");
  assert(group_of("achievement_popups") == "Notifications");
  assert(group_of("framerate_limit") == group_of("present_vsync") && group_of("present_vsync") == group_of("fps_counter"));
  assert(group_of("aim_assist") == "Aiming" && group_of("aim_toggle") == "Aiming");
  assert(group_of("vibration") == "Controller" && group_of("kbm_invert_y") == "Keyboard and mouse");
  // Pages and groups render in the order the catalog declares them, and a group's settings are together (so its
  // heading shows once). This is the order the menu draws.
  for (LauncherPage p : {LauncherPage::kGraphics, LauncherPage::kHdr, LauncherPage::kGame, LauncherPage::kControls,
                         LauncherPage::kCoop, LauncherPage::kUpdates, LauncherPage::kAbout}) {
    std::vector<std::string> groups;
    assert(PageGroups(catalog, p, &groups) && "a group's settings are not together in the catalog");
    std::string first_seen;  // concatenated, in catalog order
    for (const auto& s : catalog)
      if (s.page == p && first_seen.find("|" + s.group + "|") == std::string::npos) first_seen += "|" + s.group + "|";
    std::string drawn;
    for (const auto& g : groups) drawn += "|" + g + "|";
    assert(drawn == first_seen);
  }
  {
    std::vector<std::string> g;
    PageGroups(catalog, LauncherPage::kGame, &g);
    assert((g == std::vector<std::string>{"Movies and audio", "Effects", "Notifications"}));
    PageGroups(catalog, LauncherPage::kControls, &g);
    assert((g == std::vector<std::string>{"Aiming", "Controller", "Keyboard and mouse"}));
    PageGroups(catalog, LauncherPage::kGraphics, &g);
    assert((g == std::vector<std::string>{"Display", "Image quality", "Performance", "Renderer"}));
  }
  // A group split in two (a setting from another group between its members) is caught.
  {
    std::vector<Setting> bad(3);
    for (auto& s : bad) s.page = LauncherPage::kGame;
    bad[0].group = "A", bad[1].group = "B", bad[2].group = "A";
    assert(!PageGroups(bad, LauncherPage::kGame, nullptr));
  }

  // Section tabs wrap around; the bindings count as Controls.
  assert(NeighborPage(LauncherPage::kGraphics, -1) == LauncherPage::kAbout);
  assert(NeighborPage(LauncherPage::kAbout, 1) == LauncherPage::kGraphics);
  assert(NeighborPage(LauncherPage::kBindings, 1) == LauncherPage::kCoop);

  // Slider steps: 1-2-5 of about a fiftieth of the range.
  assert(Near(SliderStep(Slider("fov_offset", 0, 40, true)), 1.0));
  assert(Near(SliderStep(Slider("vulkan_hdr_peak", 200, 4000, true)), 100.0));
  assert(Near(SliderStep(Slider("vulkan_hdr_contrast", 0.25, 2, true)), 0.05));
  assert(Near(SliderStep(Slider("movie_volume", 0, 200, true)), 5.0));
  assert(Near(SliderStep(Slider("vulkan_hdr_paper_white", 80, 500, true)), 10.0));
  assert(Near(SliderStep(Slider("vulkan_hdr_middle_gray", 0.05, 0.5, true)), 0.01));
  assert(Near(SliderStep(Slider("vulkan_hdr_exposure", -4, 4, true)), 0.2));
  // Stepping lands on the grid and stops at the ends.
  const Setting fov = Slider("fov_offset", 0, 40, true);
  assert(Near(StepSlider(fov, 0, 1), 1));
  assert(Near(StepSlider(fov, 0, 5), 5));
  assert(Near(StepSlider(fov, 38, 5), 40));
  assert(Near(StepSlider(fov, 0, -1), 0));
  assert(Near(StepSlider(fov, 2.4, 1), 3));  // an odd value from the settings file snaps to the grid
  const Setting paper = Slider("vulkan_hdr_paper_white", 80, 500, true);
  assert(Near(StepSlider(paper, 203, 5), 250));
  // A logarithmic slider moves by a factor and keeps two decimals; from 0 it is linear.
  const Setting sens = Slider("kbm_sensitivity", 0.1, 4, false);
  assert(SliderIsLog(sens));
  const double up = StepSlider(sens, 1.0, 1);
  assert(up > 1.0 && up < 1.2 && Near(up * 100, std::round(up * 100)));
  assert(StepSlider(sens, 0.1, -1) >= 0.1);
  assert(!SliderIsLog(Slider("kbm_noise_gate", 0, 4, false)));
  // Track position and back.
  assert(Near(SliderFraction(fov, 20), 0.5));
  assert(Near(SliderFromFraction(sens, 1.0), 4.0));
  assert(Near(SnapSlider(fov, 13.4), 13));
  Setting gray = Slider("vulkan_hdr_middle_gray", 0.05, 0.5, true);
  assert(FormatSliderValue(gray, 0.18) == "0.180");
  assert(FormatSliderValue(fov, 5) == "5.00");
  gray.slider_format = "%.0f%%";
  assert(DisplaySliderValue(gray, 50) == "50%");

  // Choices: arrows stop at the ends, A wraps, a value that isn't in the list moves onto it.
  Setting c;
  c.cvar = "aspect_ratio";
  c.choices = {{"16:9", "16:9"}, {"auto", "Automatic"}, {"21:9", "21:9"}};
  c.value = "16:9";
  assert(StepChoice(c, -1, false) == "16:9");
  assert(StepChoice(c, 1, false) == "auto");
  c.value = "21:9";
  assert(StepChoice(c, 1, false) == "21:9");
  assert(StepChoice(c, 1, true) == "16:9");
  c.value = "4:3";
  assert(ChoiceIndex(c) == -1 && StepChoice(c, 1, false) == "16:9" && StepChoice(c, -1, false) == "21:9");
  assert(ValueLabel(c, "auto") == "Automatic");
  Setting b;
  assert(ValueLabel(b, "true") == "On" && ValueLabel(b, "false") == "Off");
  assert(ValuesEqual("1.000000", "1") && !ValuesEqual("1", "2") && !ValuesEqual("", "0"));

  // Prompts: the device used last; a controller's style from pad_prompts, or the pad connected.
  using bots::PromptDevice;
  assert(LauncherPromptDevice(LastInput::kKeyboardMouse, PadKind::kPlayStation5, "auto") == PromptDevice::kKeyboardMouse);
  assert(LauncherPromptDevice(LastInput::kController, PadKind::kXbox, "auto") == PromptDevice::kXbox);
  assert(LauncherPromptDevice(LastInput::kController, PadKind::kPlayStation5, "auto") == PromptDevice::kPlayStation5);
  assert(LauncherPromptDevice(LastInput::kController, PadKind::kPlayStation4, "auto") == PromptDevice::kPlayStation4);
  assert(LauncherPromptDevice(LastInput::kController, PadKind::kPlayStation5, "xbox") == PromptDevice::kXbox);
  assert(LauncherPromptDevice(LastInput::kController, PadKind::kXbox, "ps4") == PromptDevice::kPlayStation4);
  assert(LauncherPromptDevice(LastInput::kController, PadKind::kNone, "auto") == PromptDevice::kXbox);

  // Key bindings.
  const auto keys = SplitKeys("E, F,Backspace");
  assert(keys.size() == 3 && keys[0] == "E" && keys[1] == "F" && keys[2] == "Backspace");
  assert(SplitKeys("").empty());
  assert(KeyLabel("Return") == "Enter" && KeyLabel("LMB") == "Left mouse" && KeyLabel("Numpad5") == "Num 5");
  assert(KeyLabel("Q") == "Q");
  assert(BindKey("W", "Up", false) == "Up");
  assert(BindKey("W", "Up", true) == "W,Up");
  assert(BindKey("W,Up", "Up", true) == "W,Up");
  assert(BindKey("", "Up", true) == "Up");

  // The menu's scale: by height, and by width on a narrow window; 0.8 to 3.6 in steps of 0.05.
  auto near_f = [](float a, float x) { return std::fabs(a - x) < 1e-4f; };
  assert(near_f(LauncherMenuScale(1920, 1080), 1.8f));
  assert(near_f(LauncherMenuScale(1280, 800), 1.35f));   // Steam Deck
  assert(near_f(LauncherMenuScale(3840, 2160), 3.6f));   // 4K at 100%
  assert(near_f(LauncherMenuScale(1920, 1080 * 1.0f / 1.0f), 1.8f));
  assert(near_f(LauncherMenuScale(3840 / 2.0f, 2160 / 2.0f), 1.8f));  // 4K at 200%: logical 1080p
  assert(near_f(LauncherMenuScale(1024, 768), 1.15f));   // width-bound
  assert(near_f(LauncherMenuScale(1280 / 1.5f, 720 / 1.5f), 0.8f));
  assert(near_f(LauncherMenuScale(3440, 1440), 2.4f));
  assert(near_f(LauncherMenuScale(1920, 1080, "1.25"), 1.25f));
  assert(near_f(LauncherMenuScale(0, 0), 1.0f));

  // The installer's stages (idle 0, downloading 1, verifying 2, installing 3, restarting 4) as the menu counts them.
  assert(MenuUpdateStage(0, 1) == 0 && MenuUpdateStage(1, 1) == 0);
  assert(MenuUpdateStage(2, 1) == 1 && MenuUpdateStage(3, 1) == 2 && MenuUpdateStage(4, 1) == 3);

  // Failed downloads read in player language (e2e 2026-11-21: "curl 22" in the window); the raw reason stays in the log.
  for (const char* raw : {"the download failed (curl 22)", "the download failed (HTTP 404)", "the download failed (HTTP 500)"}) {
    const std::string note = UpdateFailureNote(raw);
    assert(note == "The download failed (the server returned an error). Check your connection and try again. Your install is unchanged.");
  }
  for (const char* raw : {"the download failed (curl 7)", "the download failed (curl 28)", "the download failed (curl 56)",
                          "the download failed (error 12030)"}) {  // refused, timed out, dropped mid-transfer, Windows
    const std::string note = UpdateFailureNote(raw);
    assert(note.find("the connection dropped or timed out") != std::string::npos);
    assert(note.find("curl") == std::string::npos && note.find("error 1") == std::string::npos);
  }
  assert(UpdateFailureNote("the download failed (curl 99)").find("curl") == std::string::npos);
  assert(UpdateFailureNote("the download failed (curl 23)").find("disk") != std::string::npos);
  assert(UpdateFailureNote("the new app's signature is broken") ==
         "The update didn't install (the new app's signature is broken). Your install is unchanged; the download page gets the new version by hand.");

  // Auto-save: a switch is written at once; a slider while it moves is not, on release or after 300 ms still it is.
  assert(!AutosaveDue(false, false, false, 9.0));
  assert(AutosaveDue(true, false, false, 0.0));
  assert(!AutosaveDue(true, true, false, 0.05));
  assert(AutosaveDue(true, true, true, 0.0));
  assert(AutosaveDue(true, true, false, kAutosaveIdleSeconds));
  // Why a write failed, in player language.
  assert(SaveErrorShort("Can't write /x/bots.toml (Permission denied)") == "the settings file or folder is read-only");
  assert(SaveErrorShort("Can't write /x/bots.toml (the file is read-only)") == "the settings file or folder is read-only");
  assert(SaveErrorShort("Can't write C:\\x\\bots.toml (Access is denied.)") == "the settings file or folder is read-only");
  assert(SaveErrorShort("Can't write /x/bots.toml (No space left on device)") == "the disk is full");
  assert(SaveErrorShort("Can't write /x/bots.toml (No such file or directory)") == "the settings folder can't be found");
  assert(SaveErrorShort("something else") == "the settings file can't be written");

  std::printf("launcher_logic_test: all passed\n");
  return 0;
}
