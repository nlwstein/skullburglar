// The launcher's settings list (settings.h: SettingsCatalog). Plain data with no SDK dependency, so the launcher's
// preview tool and tests (tools/launcher_preview.cpp, tests/launcher_logic_test.cpp) build it alone.
#include "settings.h"

#include <utility>

namespace bots::setup {

std::vector<Setting> SettingsCatalog() {
  std::vector<Setting> s;
  // Every setting names its page and, optionally, a group heading on that page. Pages and groups show in the order
  // they are listed here; the settings of one group sit together. Nothing is inherited from the previous entry.
  auto add = [&s](LauncherPage page, const char* group, Setting setting) {
    setting.page = page;
    setting.group = group;
    s.push_back(std::move(setting));
  };
  // Page Graphics: Display
  add(LauncherPage::kGraphics, "Display", {"fullscreen", "", "Fullscreen", "F11 toggles it in game.", {}});
  add(LauncherPage::kGraphics, "Display", {"aspect_ratio", "", "Aspect ratio",
               "Wider than 16:9 fills an ultrawide screen and shows more at the sides; the HUD and menus stretch. "
               "Automatic matches the screen in fullscreen, or the window.",
               {{"16:9", "16:9 (original)"}, {"auto", "Automatic"}, {"21:9", "21:9"}, {"32:9", "32:9"}}});
  add(LauncherPage::kGraphics, "Display", {"fov_offset", "", "Field of view",
               "Degrees added to the game's horizontal view. 0 is as shipped; more shows more of the scene and makes "
               "things look smaller. The aiming zoom and scopes widen in the same proportion; cutscenes and vehicle "
               "cameras keep the original view. F6 changes it live. Ctrl-click to type a value.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 40, s.back().slider_linear = true, s.back().slider_format = "+%.0f degrees";
  // Page Graphics: Image quality
  add(LauncherPage::kGraphics, "Image quality", {"render_scale", "", "Render resolution",
               "Higher is sharper and costs GPU time; lower is faster on weak GPUs. Automatic is 720p, or 67% on "
               "Intel graphics.",
               {{"auto", "Automatic"}, {"3", "4K  (2160p)"}, {"2", "1440p"}, {"1.5", "1080p"},
                {"1.25", "900p"}, {"1", "720p  (original)"}, {"0.75", "75%"}, {"0.667", "67%"}, {"0.5", "50%"}}});
  add(LauncherPage::kGraphics, "Image quality", {"msaa", "", "Multisampling (restart required)",
               "MSAA smooths geometry edges. SGSSAA also shades cutouts, textures and lighting per sample. "
               "Requires the new renderer.",
               {{"off", "Off"}, {"2x", "2x MSAA"}, {"4x", "4x MSAA"},
                {"2x-sgssaa", "2x SGSSAA"}, {"4x-sgssaa", "4x SGSSAA"}}});
  add(LauncherPage::kGraphics, "Image quality", {"anti_aliasing", "", "Anti-aliasing",
               "Smooths jagged edges on the 3D scene; the HUD and text are not touched. SMAA is the cleanest; FXAA is "
               "the cheapest and a little softer. Requires the new renderer.",
               {{"off", "Off"}, {"fxaa", "FXAA (fast)"}, {"smaa", "SMAA (cleaner)"}}});
  add(LauncherPage::kGraphics, "Image quality", {"sharpening", "", "Sharpening",
               "Brings back the crispness that anti-aliasing and a lower resolution take away. 0 is off. Edges that "
               "are already sharp change little. Requires the new renderer.", {}});
  s.back().slider_min = 0, s.back().slider_max = 1, s.back().slider_linear = true, s.back().slider_format = "%.2f";
  add(LauncherPage::kGraphics, "Image quality", {"ambient_occlusion", "", "Ambient occlusion",
               "Soft contact shading around nearby surfaces. Requires the new renderer.", {}});
  add(LauncherPage::kGraphics, "Image quality", {"ambient_occlusion_strength", "", "AO strength",
               "Darkens contact shading without changing its radius. 3.0 is the default.", {}});
  s.back().slider_min = 0, s.back().slider_max = 6, s.back().slider_linear = true;
  // Page Graphics: Performance
  add(LauncherPage::kGraphics, "Performance", {"framerate_limit", "", "Frame rate limit",
               "30 plays at a steady 30 fps: choose it if the game feels jumpy because your computer can't hold 60. "
               "60 is what the game was made for. 90, 120 and 144 run smoother on a high-refresh screen; the game "
               "holds 60 while you drive and while an AI-driven vehicle is in play, because vehicles drive slower at "
               "higher frame rates. Unlimited is experimental.",
               {{"30", "30 fps (steadier on weaker hardware)"}, {"60", "60 fps (original)"}, {"90", "90 fps"}, {"120", "120 fps"}, {"144", "144 fps"},
                {"0", "Unlimited (experimental)"}}});
  add(LauncherPage::kGraphics, "Performance", {"present_vsync", "", "VSync", "Off has the lowest latency but can tear.", {}});
  add(LauncherPage::kGraphics, "Performance", {"fps_counter", "", "Show FPS counter",
               "A small readout in the top-left corner: frames per second, milliseconds per frame, and which renderer "
               "is drawing. Handy for bug reports. Takes effect the next time the game starts.",
               {}});
  // Page Graphics: Renderer
  add(LauncherPage::kGraphics, "Renderer", {"renderer", "", "Renderer",
               "The new renderer draws the 3D world with the game's rebuilt scene renderer. The pre-1.0 renderer is "
               "the one earlier versions used; it stays for now in case the new one doesn't work on your PC, and will "
               "be removed. Takes effect the next time the game starts.",
               {{"new", "New renderer (recommended)"}, {"pre1", "Pre-1.0 renderer (deprecated)"}}});
  add(LauncherPage::kGraphics, "Renderer", {"pre1_occlusion", "", "Pre-1.0 renderer: hide objects behind walls",
               "Only the pre-1.0 renderer uses this. It asks the graphics card which objects are hidden behind walls "
               "and doesn't draw them. Off draws everything in view every frame, which can help if the pre-1.0 "
               "renderer shows objects popping in or out wrongly. Takes effect the next time the game starts.",
               {{"auto", "Automatic"}, {"on", "On"}, {"off", "Off (troubleshooting)"}}});
  // Page Hdr: Output
  add(LauncherPage::kHdr, "Output", {"vulkan_hdr", "", "Enable HDR (restart required)",
               "Native scRGB output on an HDR display. Requires the new renderer. "
               "Enable HDR in the operating system too. F6 opens the live display controls.", {}});
  add(LauncherPage::kHdr, "Output", {"vulkan_hdr_paper_white", "", "Game brightness (paper white)",
               "The game's reference white in nits. Highlights can extend above it.", {}});
  s.back().slider_min = 80, s.back().slider_max = 500, s.back().slider_linear = true, s.back().slider_format = "%.0f nits";
  add(LauncherPage::kHdr, "Output", {"vulkan_hdr_peak", "", "Peak brightness", "Set this to the display's peak brightness.", {}});
  s.back().slider_min = 200, s.back().slider_max = 4000, s.back().slider_linear = true, s.back().slider_format = "%.0f nits";
  add(LauncherPage::kHdr, "Output", {"vulkan_hdr_ui_white", "", "UI brightness", "HUD, menus and host overlays in nits.", {}});
  s.back().slider_min = 40, s.back().slider_max = 500, s.back().slider_linear = true, s.back().slider_format = "%.0f nits";
  // Page Hdr: Scene grade
  add(LauncherPage::kHdr, "Scene grade", {"vulkan_hdr_exposure", "", "Exposure", "Scene exposure in stops, before the HDR shoulder.", {}});
  s.back().slider_min = -4, s.back().slider_max = 4, s.back().slider_linear = true, s.back().slider_format = "%.2f EV";
  add(LauncherPage::kHdr, "Scene grade", {"vulkan_hdr_middle_gray", "", "Middle gray",
               "Scene midtones. 0.18 keeps the original curve; black and reference white stay fixed.", {}});
  s.back().slider_min = 0.05, s.back().slider_max = 0.5, s.back().slider_linear = true, s.back().slider_format = "%.3f";
  for (const auto& [name, label] : std::vector<std::pair<const char*, const char*>>{
         {"vulkan_hdr_contrast", "Contrast"}, {"vulkan_hdr_highlights", "Highlights"},
         {"vulkan_hdr_shadows", "Shadows"}, {"vulkan_hdr_saturation", "Saturation"}}) {
    add(LauncherPage::kHdr, "Scene grade", {name, "", label, "1.0 keeps the game's original grade. Adjusts the scene; the HUD is unchanged.", {}});
    s.back().slider_min = 0.25, s.back().slider_max = 2, s.back().slider_linear = true;
  }
  // Page Game: Movies and audio
  add(LauncherPage::kGame, "Movies and audio", {"intro_movies", "", "Play intro movies", "The logo movies before the title screen.", {}});
  add(LauncherPage::kGame, "Movies and audio", {"movie_volume", "", "Movie volume",
               "How loud the pre-rendered movies (story cutscenes, music videos, the title screen movie) are, as a "
               "share of their own level. 100% is as the game plays them; the slider goes up to 200%, with a soft "
               "limiter so loud parts don't distort. Game sound and music are not affected.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 200, s.back().slider_linear = true, s.back().slider_format = "%.0f%%";
  // Page Game: Effects
  add(LauncherPage::kGame, "Effects", {"motion_blur", "", "Motion blur",
               "The smear when the camera turns or things move fast. Off keeps every other effect.",
               {}});
  add(LauncherPage::kGame, "Effects", {"screen_shake", "", "Screen shake",
               "How much the camera shakes from explosions, gunfire and hits, as a share of the game's own. 100% is "
               "as shipped; 0% turns it off.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 100, s.back().slider_linear = true, s.back().slider_format = "%.0f%%";
  // Page Game: Notifications
  add(LauncherPage::kGame, "Notifications", {"achievement_popups", "", "Achievement popups",
               "Can make the game stutter while one shows. Unlocks are recorded either way.", {}});
  // Page Controls: Aiming
  add(LauncherPage::kControls, "Aiming", {"aim_assist", "", "Aim assist",
               "The game slows the crosshair and pulls it toward enemies near it while you move (even through walls, "
               "as on the Xbox 360). Turn off for raw aiming. Applies to the mouse too.",
               {}});
  add(LauncherPage::kControls, "Aiming", {"aim_toggle", "", "Toggle aim",
               "Press Aim once to start aiming and again to stop, instead of holding it. Works with a controller and "
               "with keyboard and mouse (every key bound to Aim, such as right mouse button or Shift). Taking cover "
               "or running (A) also ends it; menus, pause and vehicles reset it.",
               {}});
  // Page Controls: Controller
  add(LauncherPage::kControls, "Controller", {"pad_prompts", "", "Controller button prompts",
               "Which buttons the game shows. Automatic matches the controller you used last.",
               {{"auto", "Automatic"}, {"xbox", "Xbox"}, {"ps4", "PlayStation 4"}, {"ps5", "PlayStation 5"}}});
  add(LauncherPage::kControls, "Controller", {"right_stick_deadzone", "", "Right stick dead zone",
               "How far you push the right stick before the camera moves. 20% is the game's own, applied to each "
               "direction separately; lower values use a smaller round dead zone, so small and diagonal movements "
               "register. 0% has none, so a stick that drifts will move the camera. Steam Input's own dead zone, if "
               "you use it, still applies.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 20, s.back().slider_linear = true, s.back().slider_format = "%.0f%%";
  add(LauncherPage::kControls, "Controller", {"stick_acceleration", "", "Stick acceleration",
               "The game speeds up turning after you hold the stick fully over, and eases the camera in when you "
               "push it further, as on the Xbox 360. Turn off for constant turning speed that follows the stick at "
               "once (the top turning speed is then lower).",
               {}});
  add(LauncherPage::kControls, "Controller", {"stick_response", "", "Stick response",
               "How the stick maps to turning left and right. The original curve turns slowly for small movements. "
               "Linear turns in proportion to the stick, so small movements are faster. Up and down is always "
               "linear. The game's own Controller Sensitivity option still applies.",
               {{"original", "Original curve"}, {"linear", "Linear"}}});
  add(LauncherPage::kControls, "Controller", {"stick_look_speed", "", "Stick look speed",
               "How fast the right stick turns the camera, as a multiple of the game's own speed. 1x is as shipped; "
               "up to 3x, past the game's highest Controller Sensitivity, for a pad that turns as fast as a mouse. "
               "It scales turning left, right, up and down at every stick position, and the game's own Controller "
               "Sensitivity option still applies on top. Ctrl-click to type a value.",
               {}});
  s.back().slider_min = 1, s.back().slider_max = 3, s.back().slider_linear = true, s.back().slider_format = "%.2fx";
  add(LauncherPage::kControls, "Controller", {"vibration", "", "Vibration", "Turn off to stop the controller rumbling.", {}});
  // Page Controls: Keyboard and mouse
  add(LauncherPage::kControls, "Keyboard and mouse", {"kbm", "", "Keyboard and mouse", "Play with keyboard and mouse. A controller always works too.", {}});
  add(LauncherPage::kControls, "Keyboard and mouse", {"kbm_sensitivity", "", "Mouse sensitivity",
               "How fast the camera turns, when not aiming. 1 is the game's own speed. Ctrl-click to type a value.",
               {}});
  s.back().slider_min = 0.1, s.back().slider_max = 4;
  add(LauncherPage::kControls, "Keyboard and mouse", {"kbm_aim_sensitivity", "", "Aiming sensitivity",
               "How fast the camera turns while you hold Aim (the game already slows it as it zooms in). Ctrl-click "
               "to type a value.",
               {}});
  s.back().slider_min = 0.1, s.back().slider_max = 4;
  add(LauncherPage::kControls, "Keyboard and mouse", {"kbm_invert_y", "", "Invert mouse up/down", "", {}});
  add(LauncherPage::kControls, "Keyboard and mouse", {"kbm_noise_gate", "", "Mouse noise gate (pixels)",
               "0 = off. If the camera drifts slowly while your mouse or gyro is still, try 1 or 2: motion shorter "
               "than this is held back until it adds up. Ctrl-click to type a value.",
               {}});
  s.back().slider_min = 0, s.back().slider_max = 4;
  add(LauncherPage::kControls, "Keyboard and mouse", {"kbm_noise_gate_strict", "", "Stricter noise gate (jittery gyros)",
               "For a gyro that still twitches with the noise gate on, such as a DualSense Edge: slow motion must also "
               "keep one direction for a moment before it counts. Real turns pass as before. Needs the noise gate above 0.",
               {}});
  add(LauncherPage::kControls, "Keyboard and mouse", {"kbm_prompts", "", "Keyboard and mouse button prompts",
               "Show keys instead of controller buttons in prompts while you play with keyboard and mouse.", {}});
  // Page Coop: Over your network
  add(LauncherPage::kCoop, "Over your network", {"user_signin_live", "", "LAN co-op",
               "Play the story with another computer on your network. One player starts a story; the other picks "
               "Play Game, then Join COOP, and chooses it from the list. Both computers need this on. On Windows, "
               "allow Skullburglar through the firewall when asked, or games won't be found.",
               {}});
  // Page Coop: On one computer
  add(LauncherPage::kCoop, "On one computer", {"split_screen", "", "Split-screen co-op (experimental)",
               "One window, split top and bottom. During a story, press START on a second controller to join as "
               "player 2; player 2 leaves with Quit in their pause menu. The two-window option below overrides it.",
               {}});
  add(LauncherPage::kCoop, "On one computer", {"coop", "", "Co-op on this PC, two windows (experimental)",
               "Opens a second window for player 2. Player 1 starts a story; player 2 picks Join COOP and the game "
               "from the list.",
               {}});
  add(LauncherPage::kCoop, "On one computer", {"coop_pads", "", "Co-op controllers", "Controllers in the order they were connected.",
               {{"1,2", "P1: pad 1, P2: pad 2"}, {"2,1", "P1: pad 2, P2: pad 1"}}});
  // Page Updates: (no heading)
  add(LauncherPage::kUpdates, "", {"check_updates", "", "Check for new versions",
               "When the launcher opens, look online for a newer Skullburglar and offer its download page.", {}});
  // Page About: Diagnostics
  add(LauncherPage::kAbout, "Diagnostics", {"diagnostic_logging", "", "Diagnostic logging",
               "A fuller log for bug reports: slow frames, a system report every few seconds and GPU timings. Costs a "
               "little speed and makes the log bigger. The normal log always has the essentials. Takes effect the next "
               "time the game starts.",
               {}});
  // Key bindings: the Key bindings page (opened from Controls), one row per action.
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
    add(LauncherPage::kBindings, "", {b.cvar, "", b.label, "", {}});
    s.back().binding = true;
  }
  return s;
}

}  // namespace bots::setup
