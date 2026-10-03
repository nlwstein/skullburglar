// Keyboard and mouse driver (backlog B-104); see mnk_input.h.
#include "mnk_input.h"

#include "prompt_state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <rex/input/input.h>

REXCVAR_DEFINE_BOOL(kbm, true, "bots/Input", "Keyboard and mouse controls (mouse look locks the pointer)");
REXCVAR_DEFINE_DOUBLE(kbm_sensitivity, 1.0, "bots/Input", "Mouse look sensitivity").range(0.05, 10.0);
REXCVAR_DEFINE_BOOL(kbm_invert_y, false, "bots/Input", "Invert mouse look up/down");
REXCVAR_DEFINE_BOOL(kbm_prompts, true, "bots/Input",
                    "Show keyboard/mouse button prompts while keyboard/mouse is the last device used");

// Default layout follows Gears of War on PC, adapted to this game's actions. Each value is a
// comma-separated list of key names (rex/ui/keybinds.cpp; LMB RMB MMB, plus WheelUp WheelDown).
#define BOTS_KBM_BIND(name, keys, what) \
  REXCVAR_DEFINE_STRING(kbm_##name, keys, "bots/Input/Keys", what)
BOTS_KBM_BIND(move_forward, "W", "Move forward (left stick)");
BOTS_KBM_BIND(move_back, "S", "Move back (left stick)");
BOTS_KBM_BIND(move_left, "A", "Move left (left stick)");
BOTS_KBM_BIND(move_right, "D", "Move right (left stick)");
BOTS_KBM_BIND(right_trigger, "LMB", "Fire (RT)");
BOTS_KBM_BIND(left_trigger, "RMB", "Aim (LT)");
BOTS_KBM_BIND(a, "Space,E", "Cover / use / confirm (A)");
BOTS_KBM_BIND(b, "Shift,F,Backspace", "Special move / counter-kill / back (B)");
BOTS_KBM_BIND(x, "R", "Reload / pick up weapon (X)");
BOTS_KBM_BIND(y, "Q", "Gangsta Fury / skip (Y)");
BOTS_KBM_BIND(right_shoulder, "G,MMB", "Throw grenade (RB)");
BOTS_KBM_BIND(left_shoulder, "T", "Next grenade (LB)");
BOTS_KBM_BIND(dpad_up, "1,Up,WheelUp", "D-pad up");
BOTS_KBM_BIND(dpad_down, "3,Down,WheelDown", "D-pad down");
BOTS_KBM_BIND(dpad_left, "4,Left", "D-pad left");
BOTS_KBM_BIND(dpad_right, "2,Right", "D-pad right");
BOTS_KBM_BIND(left_thumb, "C", "Taunt (L3)");
BOTS_KBM_BIND(right_thumb, "V", "Right stick click (R3)");
BOTS_KBM_BIND(back, "Tab", "Objectives (Back)");
BOTS_KBM_BIND(start, "Escape", "Pause (Start)");
BOTS_KBM_BIND(confirm, "Return", "Menu confirm / Press START (the engine's Enter key, not a pad button)");
#undef BOTS_KBM_BIND

namespace bots {

namespace {

using rex::ui::VirtualKey;

// One wheel notch holds its D-pad direction this long: long enough for the game's 30 Hz-ish
// input sampling to see a press and a release.
constexpr auto kWheelTap = std::chrono::milliseconds(70);

// The one PromptState, published for the renderer plugin before main (prompt_state.h).
PromptState g_prompt_state;
const bool g_prompt_state_published = [] {
  char value[32];
  std::snprintf(value, sizeof(value), "%llx", static_cast<unsigned long long>(uintptr_t(&g_prompt_state)));
#ifdef _WIN32
  SetEnvironmentVariableA(kPromptStateEnv, value);
#else
  setenv(kPromptStateEnv, value, 1);
#endif
  return true;
}();

}  // namespace

void MouseKeyboardDriver::SetPromptMode(bool keyboard_mouse) {
  keyboard_mouse = keyboard_mouse && REXCVAR_GET(kbm_prompts);
  if (g_prompt_state.keyboard_mouse.exchange(keyboard_mouse) == uint32_t(keyboard_mouse)) return;
  g_prompt_state.generation.fetch_add(1);
  REXLOG_INFO("prompts: {}", keyboard_mouse ? "keyboard/mouse" : "controller");
}

void MouseKeyboardDriver::NoteGuestPad(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx,
                                       int16_t ry) {
  if (!g_prompt_state.keyboard_mouse.load(std::memory_order_relaxed)) return;
  rex::input::X_INPUT_STATE ours;
  {
    std::lock_guard lock(mutex_);
    ours = last_out_;
  }
  const auto& g = ours.gamepad;
  constexpr int kStick = 16000, kTrigger = 64;
  const bool pad = (buttons & ~g.buttons) || (lt > kTrigger && !g.left_trigger) ||
                   (rt > kTrigger && !g.right_trigger) || (!g.thumb_lx && std::abs(lx) > kStick) ||
                   (!g.thumb_ly && std::abs(ly) > kStick) || std::abs(rx) > kStick || std::abs(ry) > kStick;
  if (pad) SetPromptMode(false);
}

MouseKeyboardDriver::MouseKeyboardDriver() : InputDriver(nullptr, 0) {
  instance_.store(this, std::memory_order_release);
}

MouseKeyboardDriver::~MouseKeyboardDriver() {
  instance_.store(nullptr, std::memory_order_release);
  Detach();
}

bool MouseKeyboardDriver::Enabled() const { return REXCVAR_GET(kbm); }

void MouseKeyboardDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (!window) return;
  {
    std::lock_guard lock(mutex_);
    window_ = window;
  }
  window->AddInputListener(this, window_z_order());
  window->AddListener(this);
  REXLOG_INFO("keyboard/mouse: attached (kbm={}, sensitivity {})", Enabled(), REXCVAR_GET(kbm_sensitivity));
}

void MouseKeyboardDriver::Detach() {
  rex::ui::Window* window = window_;
  if (!window) return;
  window->app_context().CallInUIThreadSynchronous([this, window] {
    {
      std::lock_guard lock(mutex_);
      window_ = nullptr;
    }
    if (capture_queued_.load()) window->app_context().ExecutePendingFunctionsFromUIThread();
    ReleaseCaptureOnUIThread(window);
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  });
}

uint16_t MouseKeyboardDriver::ButtonKey(rex::ui::MouseEvent& e) {
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft: return uint16_t(VirtualKey::kLButton);
    case rex::ui::MouseEvent::Button::kRight: return uint16_t(VirtualKey::kRButton);
    case rex::ui::MouseEvent::Button::kMiddle: return uint16_t(VirtualKey::kMButton);
    default: return 0;
  }
}

bool MouseKeyboardDriver::MenuMode() const {
  // The HUD ticks every gameplay frame; a short gap (a hitch, a level stream) mustn't drop the pointer lock.
  constexpr int64_t kHudGapMs = 500;
  return paused_.load(std::memory_order_relaxed) || Now() - hud_tick_.load(std::memory_order_relaxed) > kHudGapMs;
}

bool MouseKeyboardDriver::Pointer(float& x, float& y, uint32_t& moves) const {
  x = pointer_x_.load(std::memory_order_relaxed);
  y = pointer_y_.load(std::memory_order_relaxed);
  moves = pointer_moves_.load(std::memory_order_relaxed);
  return x >= 0 && y >= 0 && has_focus_ && Enabled() && MenuMode();
}

void MouseKeyboardDriver::TakeEnterEvents(std::vector<bool>& out) {
  std::lock_guard lock(mutex_);
  out.swap(enter_events_);
  enter_events_.clear();
}

// Must be called with mutex_ held.
void MouseKeyboardDriver::PushEnter(uint16_t vk, bool down) {
  if (enter_down_[vk] == down) return;  // key repeat
  enter_down_[vk] = down;
  if (enter_events_.size() < 32) enter_events_.push_back(down);
}

void MouseKeyboardDriver::SetKey(uint16_t vk, bool down) {
  if (!vk || vk >= kKeyCount || !Enabled()) return;
  if (down && !has_focus_) return;
  {
    std::lock_guard lock(mutex_);
    if (vk == uint16_t(VirtualKey::kLButton)) {
      // A click that starts in a menu is Enter (confirm) until it is released, not the fire trigger.
      if (down && !key_down_[vk] && !enter_down_[vk]) click_is_enter_ = MenuMode();
      if (click_is_enter_) {
        PushEnter(vk, down);
        if (!down) click_is_enter_ = false;
        return;
      }
    }
    if (Lists(REXCVAR_GET(kbm_confirm), vk)) PushEnter(vk, down);
    key_down_[vk] = down;
  }
  if (down) SetPromptMode(true);
}

bool MouseKeyboardDriver::Lists(const std::string& keys, uint16_t vk) {
  std::string_view rest(keys);
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    std::string_view name = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    if (!name.empty() && uint16_t(rex::ui::ParseVirtualKey(name)) == vk) return true;
  }
  return false;
}

void MouseKeyboardDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!Enabled() || !has_focus_) return;
  const int32_t x = e.x(), y = e.y();
  {
    std::lock_guard lock(mutex_);
    float dx = 0, dy = 0;
    if (relative_) {
      dx = e.dx(), dy = e.dy();
    } else if (captured_) {
      dx = float(x - prev_x_), dy = float(y - prev_y_);
    }
    mouse_dx_ += dx;
    mouse_dy_ += dy;
    moved_ += std::abs(dx) + std::abs(dy);
  }
  if (moved_ > 40) {  // a deliberate move, not a nudged desk
    moved_ = 0;
    SetPromptMode(true);
  }
  prev_x_ = x;
  prev_y_ = y;
  if (!captured_ && window_) {
    const float w = float(window_->GetActualPhysicalWidth()), h = float(window_->GetActualPhysicalHeight());
    if (w > 0 && h > 0) {
      pointer_x_.store(float(x) / w, std::memory_order_relaxed);
      pointer_y_.store(float(y) / h, std::memory_order_relaxed);
      pointer_moves_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // No pointer lock on this platform: keep the cursor off the window edges.
  if (captured_ && !relative_ && window_) {
    const int32_t w = int32_t(window_->GetActualPhysicalWidth()), h = int32_t(window_->GetActualPhysicalHeight());
    if (w > 0 && h > 0 && (std::abs(x - w / 2) >= w / 4 || std::abs(y - h / 2) >= h / 4)) {
      int32_t cx = 0, cy = 0;
      if (window_->WarpMouseToCenter(cx, cy)) {
        prev_x_ = cx;
        prev_y_ = cy;
      }
    }
  }
}

void MouseKeyboardDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!Enabled() || !has_focus_ || !is_active()) return;
  // Trackpads send many fractional events; tap once per detent's worth of scrolling.
  static int32_t accum = 0;
  accum += e.scroll_y();
  const int32_t detent = rex::ui::MouseEvent::kScrollPerDetent;
  if (std::abs(accum) < detent) return;
  std::lock_guard lock(mutex_);
  wheel_until_[accum > 0 ? 0 : 1] = std::chrono::steady_clock::now() + kWheelTap;
  accum = 0;
  SetPromptMode(true);
}

void MouseKeyboardDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_ = false;
  capture_wanted_ = false;
  {
    std::lock_guard lock(mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    for (uint16_t vk = 0; vk < kKeyCount; ++vk) PushEnter(vk, false);
    click_is_enter_ = false;
    mouse_dx_ = mouse_dy_ = 0;
  }
  if (window_) ReleaseCaptureOnUIThread(window_);
}

void MouseKeyboardDriver::TakeMouseDelta(float& dx, float& dy) {
  std::lock_guard lock(mutex_);
  const float s = float(REXCVAR_GET(kbm_sensitivity));
  dx = mouse_dx_ * s;
  // Window y grows downward; the engine's MouseY is positive for "look up".
  dy = (REXCVAR_GET(kbm_invert_y) ? mouse_dy_ : -mouse_dy_) * s;
  mouse_dx_ = mouse_dy_ = 0;
}

// Must be called with mutex_ held.
bool MouseKeyboardDriver::Pressed(const std::string& keys) const {
  const auto now = std::chrono::steady_clock::now();
  std::string_view rest(keys);
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    std::string_view name = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    if (name == "WheelUp") {
      if (now < wheel_until_[0]) return true;
      continue;
    }
    if (name == "WheelDown") {
      if (now < wheel_until_[1]) return true;
      continue;
    }
    const auto vk = uint16_t(rex::ui::ParseVirtualKey(name));
    if (vk && vk < kKeyCount && key_down_[vk]) return true;
  }
  return false;
}

void MouseKeyboardDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  if (!Enabled()) return;
  rex::input::DeviceInfo info;
  info.id = kDevice;
  info.name = "Keyboard and Mouse";
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT MouseKeyboardDriver::GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) {
  if (!Enabled() || id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  const bool live = has_focus_ && is_active();
  const bool menu = MenuMode();
  if (menu != last_menu_) {
    last_menu_ = menu;
    REXLOG_INFO("keyboard/mouse: {}", menu ? "menu (pointer free, click = Enter)" : "gameplay (pointer locked)");
  }
  QueueCapture(live && !menu);
  if (!out) return X_ERROR_SUCCESS;
  std::memset(out, 0, sizeof(*out));
  if (live) {
    using namespace rex::input;
    static const struct {
      const std::string& (*keys)();
      uint16_t mask;
    } kButtons[] = {
        {[]() -> const std::string& { return REXCVAR_GET(kbm_a); }, X_INPUT_GAMEPAD_A},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_b); }, X_INPUT_GAMEPAD_B},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_x); }, X_INPUT_GAMEPAD_X},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_y); }, X_INPUT_GAMEPAD_Y},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_left_shoulder); }, X_INPUT_GAMEPAD_LEFT_SHOULDER},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_right_shoulder); }, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_dpad_up); }, X_INPUT_GAMEPAD_DPAD_UP},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_dpad_down); }, X_INPUT_GAMEPAD_DPAD_DOWN},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_dpad_left); }, X_INPUT_GAMEPAD_DPAD_LEFT},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_dpad_right); }, X_INPUT_GAMEPAD_DPAD_RIGHT},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_left_thumb); }, X_INPUT_GAMEPAD_LEFT_THUMB},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_right_thumb); }, X_INPUT_GAMEPAD_RIGHT_THUMB},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_back); }, X_INPUT_GAMEPAD_BACK},
        {[]() -> const std::string& { return REXCVAR_GET(kbm_start); }, X_INPUT_GAMEPAD_START},
    };
    std::lock_guard lock(mutex_);
    uint16_t buttons = 0;
    for (const auto& b : kButtons)
      if (Pressed(b.keys())) buttons |= b.mask;
    out->gamepad.buttons = buttons;
    out->gamepad.left_trigger = Pressed(REXCVAR_GET(kbm_left_trigger)) ? 0xFF : 0;
    out->gamepad.right_trigger = Pressed(REXCVAR_GET(kbm_right_trigger)) ? 0xFF : 0;
    int32_t lx = 0, ly = 0;
    if (Pressed(REXCVAR_GET(kbm_move_left))) lx -= 32767;
    if (Pressed(REXCVAR_GET(kbm_move_right))) lx += 32767;
    if (Pressed(REXCVAR_GET(kbm_move_forward))) ly += 32767;
    if (Pressed(REXCVAR_GET(kbm_move_back))) ly -= 32767;
    out->gamepad.thumb_lx = int16_t(lx);
    out->gamepad.thumb_ly = int16_t(ly);
  }
  const auto& g = out->gamepad;
  const uint64_t sig = g.buttons | uint64_t(g.left_trigger) << 16 | uint64_t(g.right_trigger) << 24 |
                       uint64_t(uint16_t(g.thumb_lx)) << 32 | uint64_t(uint16_t(g.thumb_ly)) << 48;
  if (sig != last_sig_) {
    last_sig_ = sig;
    ++packet_;
  }
  out->packet_number = packet_;
  std::lock_guard lock(mutex_);
  last_out_ = *out;
  return X_ERROR_SUCCESS;
}

X_RESULT MouseKeyboardDriver::GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                                    rex::input::X_INPUT_CAPABILITIES* caps) {
  if (!Enabled() || id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (caps) {
    std::memset(caps, 0, sizeof(*caps));
    caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    caps->gamepad.buttons = 0xFFFF;
    caps->gamepad.left_trigger = 0xFF;
    caps->gamepad.right_trigger = 0xFF;
    caps->gamepad.thumb_lx = caps->gamepad.thumb_ly = int16_t(0x7FFF);
    caps->gamepad.thumb_rx = caps->gamepad.thumb_ry = int16_t(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MouseKeyboardDriver::SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) {
  return Enabled() && id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT MouseKeyboardDriver::GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                                 rex::input::X_INPUT_KEYSTROKE*) {
  return Enabled() && id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

// Called on the guest thread; the capture itself must happen on the UI thread (SDL).
void MouseKeyboardDriver::QueueCapture(bool want) {
  capture_wanted_.store(want, std::memory_order_relaxed);
  bool queued = false;
  if (!capture_queued_.compare_exchange_strong(queued, true)) return;
  std::lock_guard lock(mutex_);
  if (!window_) {
    capture_queued_ = false;
    return;
  }
  window_->app_context().CallInUIThreadDeferred([this] {
    capture_queued_ = false;
    ApplyCaptureOnUIThread();
  });
}

void MouseKeyboardDriver::ApplyCaptureOnUIThread() {
  rex::ui::Window* window = window_;
  if (!window) return;
  const bool want = capture_wanted_.load(std::memory_order_relaxed);
  if (want == captured_) return;
  if (!want) {
    ReleaseCaptureOnUIThread(window);
    return;
  }
  captured_ = true;
  saved_cursor_ = window->GetCursorVisibility();
  window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
  window->CaptureMouse();
  relative_ = window->SetRelativeMouseMode(true);
  if (!relative_) REXLOG_WARN("keyboard/mouse: no pointer lock, recentering the cursor instead");
  std::lock_guard lock(mutex_);
  mouse_dx_ = mouse_dy_ = 0;  // no jump on capture
}

void MouseKeyboardDriver::ReleaseCaptureOnUIThread(rex::ui::Window* window) {
  if (!captured_) return;
  captured_ = false;
  window->SetCursorVisibility(saved_cursor_);
  window->SetRelativeMouseMode(false);
  relative_ = false;
  window->ReleaseMouse();
}

}  // namespace bots
