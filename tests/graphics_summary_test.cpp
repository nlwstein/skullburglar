// Unit tests for src/graphics_summary.h (the "summary: graphics:" line). CI `checks` builds and runs this with the
// launcher's settings catalog: `c++ ... tests/graphics_summary_test.cpp src/setup/settings_catalog.cpp`.
#include <cstdio>
#include <set>
#include <string>

#include "graphics_summary.h"
#include "setup/settings.h"

using namespace bots::diag;

static int failures = 0;
#define CHECK(c)                                                                       \
  do {                                                                                 \
    if (!(c)) std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c), ++failures;    \
  } while (0)
#define CHECK_HAS(text, part)                                                                                       \
  do {                                                                                                              \
    if ((text).find(part) == std::string::npos)                                                                     \
      std::fprintf(stderr, "FAIL line %d: %s\n  does not contain: %s\n  text: %s\n", __LINE__, #text, part, (text).c_str()), ++failures; \
  } while (0)

int main() {
  const GraphicsApplied defaults;
  const std::string line = FormatGraphicsSummary(defaults);
  CHECK(line.rfind("summary: graphics: ", 0) == 0);
  CHECK(line.find('\n') == std::string::npos);

  // Every graphics setting the launcher writes to bots.toml has a field named after its key, and every field is a launcher
  // setting: a new Graphics or HDR page setting fails this until the line reports it (add it to GraphicsFields).
  {
    std::set<std::string> fields;
    for (const auto& [key, value] : GraphicsFields(defaults)) {
      CHECK(!value.empty());
      CHECK(fields.insert(key).second);  // no key twice
    }
    std::set<std::string> launcher;
    for (const auto& s : bots::setup::SettingsCatalog()) {
      using bots::setup::LauncherPage;
      // The Graphics and HDR pages, plus the one graphics setting that sits on the Game page (Effects).
      if (s.page == LauncherPage::kGraphics || s.page == LauncherPage::kHdr || s.cvar == "motion_blur") launcher.insert(s.cvar);
    }
    CHECK(launcher.size() >= 20);  // the catalog really was read
    for (const std::string& key : launcher) {
      if (!fields.count(key)) std::fprintf(stderr, "launcher graphics setting without a summary field: %s\n", key.c_str()), ++failures;
      CHECK_HAS(line, (key + "=").c_str());
    }
    for (const std::string& key : fields)
      if (!launcher.count(key)) std::fprintf(stderr, "summary field that is not a launcher graphics setting: %s\n", key.c_str()), ++failures;
  }

  // Render resolution: the setting, the scale in effect and the size the game is drawn at.
  {
    GraphicsApplied g;
    g.render_scale_setting = "auto", g.scale = 2.0f / 3.0f;
    g.internal_w = 853, g.internal_h = 480, g.window_w = 1920, g.window_h = 1080;
    const std::string t = RenderScaleText(g);
    CHECK_HAS(t, "auto -> 0.667x");
    CHECK_HAS(t, "drawn at 853x480");
    CHECK_HAS(t, "window 1920x1080 (upscaled)");
    g.render_scale_setting = "3", g.scale = 3.0f, g.internal_w = 3840, g.internal_h = 2160, g.window_w = 1920, g.window_h = 1080;
    CHECK_HAS(RenderScaleText(g), "(scaled down)");
    g.render_scale_setting = "1.5", g.render_scale_env = "2", g.scale = 2.0f;
    CHECK_HAS(RenderScaleText(g), "BOTS_GFX_RENDER_SCALE=2 wins");
    g = GraphicsApplied{};
    g.render_scale_setting = "9", g.scale = 1.0f;
    CHECK_HAS(RenderScaleText(g), "not a usable scale, so 1x");
    g = GraphicsApplied{};
    g.render_scale_setting = "2", g.scale = 1.5f;
    CHECK_HAS(RenderScaleText(g), "changed after the game started: applies next start");
    g.scale = 2.0f;
    CHECK(RenderScaleText(g).find("changed after") == std::string::npos);
    g = GraphicsApplied{};
    CHECK_HAS(RenderScaleText(g), "drawn at not known yet");
    CHECK(RenderScaleText(g).find("window") == std::string::npos);
  }

  // Changed requests: each says what it became and why.
  {
    GraphicsApplied g;
    g.hdr_requested = true, g.hdr_active = false;
    CHECK_HAS(HdrText(g), "off (requested, but the display gave no HDR");
    g.hdr_active = true;
    CHECK_HAS(HdrText(g), "on (requested, active)");
    g.hdr_requested = true, g.hdr_active = false, g.n1_active = false;
    CHECK_HAS(HdrText(g), "needs the new renderer");
    g.hdr_requested = false;
    CHECK(HdrText(g) == "off");

    g = GraphicsApplied{};
    g.n1_active = false, g.renderer_setting = "pre1";
    CHECK(RendererText(g) == "pre-1.0 (setting)");
    CHECK_HAS(AntiAliasingText(g), "off (needs the new renderer; setting is smaa)");
    CHECK_HAS(PauseBlurText(g), "needs the new renderer");
    g.ambient_occlusion_setting = true;
    CHECK_HAS(AmbientOcclusionText(g), "needs the new renderer");
    g.n1_built = false, g.renderer_setting = "new";
    CHECK_HAS(RendererText(g), "this build has no new renderer");

    g = GraphicsApplied{};
    g.ambient_occlusion_setting = true, g.n1_reported = true, g.n1_ao_failed = true;
    CHECK_HAS(AmbientOcclusionText(g), "off (setup failed");
    g.n1_ao_failed = false, g.n1_ao_active = true;
    CHECK(AmbientOcclusionText(g) == "on");
    g.anti_aliasing_setting = "smaa", g.n1_aa_mode = 0;
    CHECK_HAS(AntiAliasingText(g), "off (setting is smaa, applies at the next 3D frame)");
    g.n1_aa_mode = 2;
    CHECK(AntiAliasingText(g) == "smaa");
    g.pause_blur_setting = true, g.n1_pause_blur = false;
    CHECK_HAS(PauseBlurText(g), "BOTS_N1_PAUSE_FX=0");
    g.pause_blur_setting = false;
    CHECK(PauseBlurText(g) == "off");

    g = GraphicsApplied{};
    g.present_vsync = true;
    CHECK_HAS(PresentText(g), "on (FIFO)");
    g.present_vsync = false;
    CHECK_HAS(PresentText(g), "IMMEDIATE");
    g.present_mode_from_launch = true;
    CHECK_HAS(PresentText(g), "launch option");
    g.framerate_limit = 0;
    CHECK(FramerateText(g) == "unlimited");
  }

  // Numbers: no trailing zeros, no "-0".
  CHECK(Num(1.5) == "1.5");
  CHECK(Num(200) == "200");
  CHECK(Num(0.667) == "0.667");
  CHECK(Num(3.0) == "3");
  CHECK(Num(-0.0001) == "0");
  CHECK(Num(10000) == "10000");

  // The log gate: the first line goes out; a repeat doesn't; a change waits out the gap and then goes (the last state is
  // always logged because the caller offers it again).
  {
    GraphicsLogGate gate(2.0);
    CHECK(gate.ShouldLog("a", 10.0));
    CHECK(!gate.ShouldLog("a", 11.0));
    CHECK(!gate.ShouldLog("b", 11.0));  // changed, but the last line is under 2 s old
    CHECK(!gate.ShouldLog("b", 11.9));
    CHECK(gate.ShouldLog("b", 12.0));
    CHECK(!gate.ShouldLog("b", 30.0));
    CHECK(gate.ShouldLog("a", 40.0));  // changing back is a change
  }

  if (failures) return 1;
  std::puts("graphics_summary_test: ok");
  return 0;
}
