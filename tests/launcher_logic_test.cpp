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
  // Every setting in the catalog is on exactly one page (not the main menu), and every page but the bindings and the
  // files page has settings. A new setting with an unknown name still lands somewhere, by its section.
  const auto catalog = SettingsCatalog();
  const auto sections = SectionsOf(catalog);
  std::set<LauncherPage> used;
  for (size_t i = 0; i < catalog.size(); ++i) {
    const LauncherPage p = PageOf(catalog[i], sections[i]);
    assert(p != LauncherPage::kMain);
    used.insert(p);
    if (catalog[i].binding) assert(p == LauncherPage::kBindings);
  }
  for (LauncherPage p : {LauncherPage::kGraphics, LauncherPage::kHdr, LauncherPage::kGame, LauncherPage::kControls,
                         LauncherPage::kCoop, LauncherPage::kUpdates, LauncherPage::kAbout, LauncherPage::kBindings})
    assert(used.count(p));
  auto page_of = [&](const char* cvar) {
    for (size_t i = 0; i < catalog.size(); ++i)
      if (catalog[i].cvar == cvar) return PageOf(catalog[i], sections[i]);
    assert(false && "not in the catalog");
    return LauncherPage::kMain;
  };
  assert(page_of("fullscreen") == LauncherPage::kGraphics);
  assert(page_of("fov_offset") == LauncherPage::kGraphics);
  assert(page_of("vulkan_hdr_peak") == LauncherPage::kHdr);
  assert(page_of("movie_volume") == LauncherPage::kGame);
  assert(page_of("kbm_sensitivity") == LauncherPage::kControls);
  assert(page_of("pad_prompts") == LauncherPage::kControls);
  assert(page_of("coop_pads") == LauncherPage::kCoop);
  assert(page_of("check_updates") == LauncherPage::kUpdates);
  assert(page_of("diagnostic_logging") == LauncherPage::kAbout);
  Setting future;
  future.cvar = "some_new_option";
  assert(PageOf(future, "Co-op") == LauncherPage::kCoop);
  assert(PageOf(future, "Something else") == LauncherPage::kGame);
  assert(PageOf(future, "") == LauncherPage::kGraphics);

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

  std::printf("launcher_logic_test: all passed\n");
  return 0;
}
