// Ultrawide (GitHub #1); see widescreen.h. The four mid-asm hooks (config/default.toml) are the same places the
// Xenia game patch for this title changes, with the aspect ratio worked out at run time instead of fixed at 21:9.
#include "widescreen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ui/flags.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "bots/Display",
                      "Shape of the picture: 16:9 (the original), auto (match the window or screen), or a wider "
                      "ratio such as 21:9 or 32:9. Wider than 16:9 shows more at the sides; the HUD and menus "
                      "stretch. Applies when the game starts. Native renderer only");

namespace bots {
namespace {

constexpr float kWidth = 1280.0f;
constexpr float k16x9 = 16.0f / 9.0f;
constexpr float kWidest = 32.0f / 9.0f;

std::atomic<rex::ui::Window*> g_window{nullptr};
std::once_flag g_once;
std::atomic<uint32_t> g_height{720};
std::atomic<bool> g_decided{false};

// "21:9", "2.37", "auto"; 0 when it can't be read.
float Requested() {
  std::string s = REXCVAR_GET(aspect_ratio);
  if (s == "auto") {
    rex::ui::Window* w = g_window.load();
    const uint32_t ww = w ? w->GetActualPhysicalWidth() : 0, wh = w ? w->GetActualPhysicalHeight() : 0;
    REXLOG_INFO("widescreen: auto, window {}x{}", ww, wh);
    return ww && wh ? float(ww) / float(wh) : 0.0f;
  }
  const size_t colon = s.find(':');
  if (colon == std::string::npos) return std::strtof(s.c_str(), nullptr);
  const float a = std::strtof(s.c_str(), nullptr), b = std::strtof(s.c_str() + colon + 1, nullptr);
  return b > 0 ? a / b : 0.0f;
}

void Decide() {
  struct Done {
    ~Done() { g_decided.store(true); }
  } done;
  if (rex::cvar::GetFlagByName("gpu_plugin") != "native") {  // the emulated GPU's presenter would squeeze it
    REXLOG_INFO("widescreen: 16:9 (aspect_ratio needs the native renderer)");
    return;
  }
  const float want = Requested();
  // Only wider than 16:9 (a 16:10 or 4:3 screen keeps the original picture, pillarboxed as before), at most 32:9,
  // and not for a sliver: under 2% wider is 16:9.
  if (!(want > k16x9 * 1.02f)) {
    REXLOG_INFO("widescreen: 16:9 (aspect_ratio {})", REXCVAR_GET(aspect_ratio));
    return;
  }
  const float aspect = std::min(want, kWidest);
  g_height = std::max(2u, uint32_t(std::lround(kWidth / aspect / 2.0f)) * 2);  // even, like every 360 target
  REXLOG_INFO("widescreen: {:.3f}:1 (aspect_ratio {}), back buffer 1280x{}", kWidth / float(g_height.load()),
              REXCVAR_GET(aspect_ratio), g_height.load());
}

bool Wide() { return GuestHeight() != 720; }

// Horizontal widening of the field of view: tan(new/2) = tan(old/2) * k.
float FovScale() { return (kWidth / float(GuestHeight())) / k16x9; }

}  // namespace

void SetWidescreenWindow(rex::ui::Window* window) { g_window.store(window); }

uint32_t GuestHeight() {
  std::call_once(g_once, Decide);
  return g_height;
}

void WindowToPicture(float x, float y, float w, float h, float& fx, float& fy) {
  float left = 0, top = 0, pw = w, ph = h;
  if (REXCVAR_GET(present_letterbox)) {  // off: the picture is stretched over the whole window
    // Not GuestHeight(): a pointer move before the window is known must not decide "auto" (16:9 until decided).
    const float aspect = kWidth / float(g_decided.load() ? g_height.load() : 720u);
    if (w > h * aspect) pw = h * aspect, left = (w - pw) / 2;
    else ph = w / aspect, top = (h - ph) / 2;
  }
  fx = std::clamp((x - left) / pw, 0.0f, 1.0f);
  fy = std::clamp((y - top) / ph, 0.0f, 1.0f);
}

}  // namespace bots

// 0x8256AC28 (stw r11,0x4060(r29)): the device setup stores the back buffer height (li r11,720) next to its width
// (1280). Every later reader (D3D device, render targets, viewport, the aspect below) follows the global.
void BotsWideBackBufferHeight(PPCRegister& r11) {
  if (r11.u32 == 720) r11.u32 = bots::GuestHeight();
}

// 0x82BC8FB8 (stfs f0,0x914(r31) follows): the viewport's aspect ratio, 16:9 from a constant when the console is in
// widescreen mode, else width / height of the back buffer. Both become the wide ratio.
void BotsWideAspect(PPCRegister& f0) {
  if (bots::Wide()) f0.f64 = double(1280.0f / float(bots::GuestHeight()));
}

// 0x825CDAF8 (beq to 0x825CDBAC): a camera that constrains its aspect ratio (cutscene cameras; +0x218 bit, ratio
// at +0x220) builds its own 16:9 projection, which would stretch across a wider frame. Wide: take the normal
// projection like any other camera.
bool BotsWideSkipConstrainedCamera() { return bots::Wide(); }

// 0x82F62E5C: the end of the player's FOV function (sub_82F62DA0, degrees in f1; it already adapts the FOV to a
// split-screen viewport). Keep the vertical FOV of 16:9 and widen the horizontal one (Hor+).
void BotsWideFov(PPCRegister& f1) {
  if (!bots::Wide()) return;
  constexpr double kPi = 3.14159265358979323846;
  const double half = f1.f64 * kPi / 360.0;
  if (!(half > 0.0 && half < kPi / 2)) return;
  f1.f64 = std::atan(std::tan(half) * double(bots::FovScale())) * 360.0 / kPi;
}
