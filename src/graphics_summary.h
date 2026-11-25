// The "summary: graphics:" line (the diagnostics file's answer to "what graphics settings was this run really using?").
//
// One line with every player-facing graphics setting AS APPLIED, not as requested: the resolution the renderer actually
// draws at, the renderer that is really on, whether HDR came up, whether AO or the pause blur is on in the renderer and
// not only in bots.toml, and the reason when a request was changed or dropped ("hdr off (display has no HDR output)").
// Each field is named after the launcher's bots.toml key, so a report reads like the config the player edited.
// tests/graphics_summary_test.cpp fails when a launcher setting on the Graphics or HDR page has no field here.
//
// This header is the pure part (no SDK, no OS): the caller (Renderer::NoteGraphics, src/gfx/vk/present.cpp) gathers
// `GraphicsApplied` from the cvars and from what the renderer reports, then logs `FormatGraphicsSummary` whenever it
// changes (`GraphicsLogGate` rate-limits that, so dragging a slider or resizing the window does not flood the log).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bots::diag {

// Everything the line says. Plain values; the caller fills them from the live state. "Setting" is what the player asked
// for (bots.toml, launcher, flag or environment, whichever won); the unprefixed fields are what is in effect.
struct GraphicsApplied {
  // Renderer.
  std::string renderer_setting = "new";  // the `renderer` setting
  bool n1_built = true;                  // this build has the new renderer
  bool n1_active = true;                 // the new renderer is the one drawing
  // Render resolution.
  std::string render_scale_setting = "auto";  // the `render_scale` setting as written ("auto", "1.5", ...)
  std::string render_scale_env;               // BOTS_GFX_RENDER_SCALE when set (it wins over the setting)
  float scale = 1.0f;                         // the scale in effect (1 = 1280x720)
  uint32_t internal_w = 0, internal_h = 0;    // the size the game is drawn at (0: not known yet)
  uint32_t window_w = 0, window_h = 0;        // the window's drawable size (what the picture is scaled to)
  bool window_known = false;                  // window_w/h and `fullscreen` come from the window itself
  // Picture shape and field of view.
  std::string aspect_setting = "16:9";
  uint32_t guest_height = 720;  // the back buffer is 1280 x this (widescreen.cpp)
  double fov_offset_setting = 0;
  // Image quality (the new renderer's filters).
  std::string anti_aliasing_setting = "smaa";
  double sharpening_setting = 0;
  bool ambient_occlusion_setting = false;
  double ambient_occlusion_strength = 3.0;
  // What the new renderer reports it applied (valid only when n1_reported).
  bool n1_reported = false;
  uint32_t n1_aa_mode = 0;  // 0 off, 1 FXAA, 2 SMAA
  double n1_sharpen = 0;
  bool n1_ao_active = false;
  bool n1_ao_failed = false;
  bool n1_pause_blur = true;
  // Performance.
  uint32_t framerate_limit = 60;  // 0 = unlimited
  bool present_vsync = true;
  bool present_mode_from_launch = false;  // a vulkan_allow_present_mode_* flag or environment value decides, not the setting
  bool fps_counter = false;
  bool pause_blur_setting = true;
  // HDR.
  bool hdr_requested = false;  // the `vulkan_hdr` setting as the player left it (before the pre-1.0 renderer forces it off)
  bool hdr_active = false;     // the game's output really is HDR (FP16 scRGB)
  double hdr_paper_white = 200, hdr_peak = 1000, hdr_ui_white = 200;
  double hdr_exposure = 0, hdr_middle_gray = 0.18, hdr_contrast = 1, hdr_highlights = 1, hdr_shadows = 1, hdr_saturation = 1;
  // Other.
  bool fullscreen = false;
  bool motion_blur = true;
  std::string pre1_occlusion = "auto";
};

// "1.5", "0.667", "200", "0": at most three decimals, no trailing zeros.
inline std::string Num(double v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.3f", v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s == "-0" ? "0" : s;
}

inline std::string OnOff(bool v) { return v ? "on" : "off"; }

inline const char* AaName(uint32_t mode) { return mode == 1 ? "fxaa" : mode == 2 ? "smaa" : "off"; }

// The renderer that is in effect, and why when it is not the one the player picked.
inline std::string RendererText(const GraphicsApplied& g) {
  if (g.n1_active) return "new (n1)";
  if (!g.n1_built) return "pre-1.0 (this build has no new renderer; setting is " + g.renderer_setting + ")";
  return g.renderer_setting == "pre1" ? "pre-1.0 (setting)" : "pre-1.0 (the new renderer could not be used; setting is " + g.renderer_setting + ")";
}

inline std::string SizeText(uint32_t w, uint32_t h) { return w && h ? std::to_string(w) + "x" + std::to_string(h) : std::string("not known yet"); }

inline std::string RenderScaleText(const GraphicsApplied& g) {
  std::string t = g.render_scale_setting;
  if (!g.render_scale_env.empty()) t += " (BOTS_GFX_RENDER_SCALE=" + g.render_scale_env + " wins)";
  t += " -> " + Num(g.scale) + "x";
  // A setting the renderer could not use (outside 0.25 to 3) ends up at 1.
  if (g.render_scale_env.empty() && g.render_scale_setting != "auto") {
    const double asked = std::atof(g.render_scale_setting.c_str());
    if (!(asked >= 0.25 && asked <= 3.0)) t += " (not a usable scale, so 1x)";
    else if (g.scale > 0 && (asked - g.scale > 0.01 || g.scale - asked > 0.01)) t += " (changed after the game started: applies next start)";
  }
  t += ", drawn at " + SizeText(g.internal_w, g.internal_h);
  if (g.window_w && g.window_h) {
    t += ", window " + SizeText(g.window_w, g.window_h);
    if (g.internal_w && (g.internal_w != g.window_w || g.internal_h != g.window_h))
      t += g.internal_w < g.window_w ? " (upscaled)" : " (scaled down)";
  }
  return t;
}

inline std::string AspectText(const GraphicsApplied& g) {
  const double ratio = g.guest_height ? 1280.0 / double(g.guest_height) : 0;
  return g.aspect_setting + " -> back buffer 1280x" + std::to_string(g.guest_height) + " (" + Num(ratio) + ":1)";
}

// A new-renderer-only setting: the value in effect, or why it is off.
inline std::string NeedsN1(const GraphicsApplied& g, const std::string& on_text) {
  return g.n1_active ? on_text : "off (needs the new renderer)";
}

inline std::string AntiAliasingText(const GraphicsApplied& g) {
  if (!g.n1_active) return "off (needs the new renderer; setting is " + g.anti_aliasing_setting + ")";
  if (!g.n1_reported) return g.anti_aliasing_setting + " (setting; the renderer has not drawn a 3D frame yet)";
  const std::string applied = AaName(g.n1_aa_mode);
  // The renderer reads the setting at each 3D frame, so after a change in the menus it applies when the next scene is drawn.
  return applied == g.anti_aliasing_setting ? applied : applied + " (setting is " + g.anti_aliasing_setting + ", applies at the next 3D frame)";
}

inline std::string SharpeningText(const GraphicsApplied& g) {
  if (!g.n1_active) return "off (needs the new renderer)";
  const double v = g.n1_reported ? g.n1_sharpen : g.sharpening_setting;
  return (v >= 0.01 ? Num(v) : std::string("off")) + (g.n1_reported ? "" : " (setting; no 3D frame drawn yet)");
}

inline std::string AmbientOcclusionText(const GraphicsApplied& g) {
  if (!g.ambient_occlusion_setting) return "off";
  if (!g.n1_active) return "off (needs the new renderer)";
  if (g.n1_reported && g.n1_ao_failed) return "off (setup failed on this GPU; see the 'ambient occlusion' line)";
  return "on";
}

inline std::string PresentText(const GraphicsApplied& g) {
  std::string t = OnOff(g.present_vsync);
  if (g.present_mode_from_launch) return t + " (a vulkan_allow_present_mode launch option decides the mode; see the 'VulkanPresenter: Created' line)";
  // vsync on allows only FIFO (always available); off prefers IMMEDIATE and the driver may have to pick another.
  return t + (g.present_vsync ? " (FIFO)" : " (IMMEDIATE if the driver has it, else MAILBOX or FIFO; the 'VulkanPresenter: Created' line has the mode used)");
}

inline std::string PauseBlurText(const GraphicsApplied& g) {
  if (!g.pause_blur_setting) return "off";
  if (!g.n1_active) return "off (needs the new renderer)";
  if (g.n1_reported && !g.n1_pause_blur) return "off (BOTS_N1_PAUSE_FX=0)";
  return "on";
}

// HDR requested against HDR in effect, with the reason when they differ.
inline std::string HdrText(const GraphicsApplied& g) {
  if (!g.hdr_requested) return "off";
  if (g.hdr_active) return "on (requested, active)";
  if (!g.n1_active) return "off (requested, but HDR needs the new renderer)";
  return "off (requested, but the display gave no HDR (scRGB) output: SDR for this run)";
}

inline std::string FramerateText(const GraphicsApplied& g) {
  return g.framerate_limit ? std::to_string(g.framerate_limit) : std::string("unlimited");
}

// Every field, in the order the line shows them, named after the bots.toml key (the launcher writes these keys).
inline std::vector<std::pair<std::string, std::string>> GraphicsFields(const GraphicsApplied& g) {
  const bool hdr = g.hdr_active;
  return {
      {"renderer", RendererText(g)},
      {"render_scale", RenderScaleText(g)},
      {"aspect_ratio", AspectText(g)},
      {"fov_offset", "+" + Num(g.fov_offset_setting) + " degrees"},
      {"anti_aliasing", AntiAliasingText(g)},
      {"sharpening", SharpeningText(g)},
      {"ambient_occlusion", AmbientOcclusionText(g)},
      {"ambient_occlusion_strength", Num(g.ambient_occlusion_strength)},
      {"framerate_limit", FramerateText(g)},
      {"present_vsync", PresentText(g)},
      {"fps_counter", OnOff(g.fps_counter)},
      {"pause_blur", PauseBlurText(g)},
      {"fullscreen", OnOff(g.fullscreen)},
      {"motion_blur", OnOff(g.motion_blur)},
      {"pre1_occlusion", g.pre1_occlusion + (g.n1_active ? " (only the pre-1.0 renderer uses it)" : "")},
      {"vulkan_hdr", HdrText(g)},
      {"vulkan_hdr_paper_white", Num(g.hdr_paper_white) + " nits" + (hdr ? "" : " (not used: HDR is off)")},
      {"vulkan_hdr_peak", Num(g.hdr_peak) + " nits"},
      {"vulkan_hdr_ui_white", Num(g.hdr_ui_white) + " nits"},
      {"vulkan_hdr_exposure", Num(g.hdr_exposure) + " EV"},
      {"vulkan_hdr_middle_gray", Num(g.hdr_middle_gray)},
      {"vulkan_hdr_contrast", Num(g.hdr_contrast)},
      {"vulkan_hdr_highlights", Num(g.hdr_highlights)},
      {"vulkan_hdr_shadows", Num(g.hdr_shadows)},
      {"vulkan_hdr_saturation", Num(g.hdr_saturation)},
  };
}

// "summary: graphics: renderer=new (n1); render_scale=auto -> 0.667x, ...; ..." (one line, no newline).
inline std::string FormatGraphicsSummary(const GraphicsApplied& g) {
  std::string line = "summary: graphics: ";
  bool first = true;
  for (const auto& [key, value] : GraphicsFields(g)) {
    if (!first) line += "; ";
    first = false;
    line += key + "=" + value;
  }
  return line;
}

// Decides when the line goes to the log: at once the first time, then only when it changed and the last one is at least
// `min_gap_s` old (a changed line that has to wait is offered again on the next check, so the last state is always logged).
class GraphicsLogGate {
 public:
  explicit GraphicsLogGate(double min_gap_s = 2.0) : min_gap_s_(min_gap_s) {}
  bool ShouldLog(const std::string& line, double now_s) {
    if (logged_ && line == last_) return false;
    if (logged_ && now_s - last_time_ < min_gap_s_) return false;
    logged_ = true;
    last_ = line;
    last_time_ = now_s;
    return true;
  }

 private:
  double min_gap_s_;
  bool logged_ = false;
  std::string last_;
  double last_time_ = 0;
};

}  // namespace bots::diag
