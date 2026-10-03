// Keyboard and mouse (backlog B-104).
//
// Keys and mouse buttons drive a synthetic controller (merged into guest user 0, so a real pad works
// alongside): each pad control has a `kbm_*` cvar listing its keys. Mouse motion does NOT go through
// the right stick: the game's stick path applies C1PlayerInput's power curve and turn lag, which feels
// wrong for a mouse. Instead game_hooks.cpp drains TakeMouseDelta() each frame and feeds the engine's
// own PC MouseX / MouseY axes (linear, after the stick curve), as UE3 does on PC.
//
// The pointer is locked while the window has focus, no settings overlay is open (is_active()) and the game isn't
// in a menu (MenuMode()). The mouse wheel taps D-pad up/down (weapon select). Keys bound to kbm_confirm (and a left
// click in menus) send the engine's own Enter key instead of a pad button.
//
// Button prompts (B-105): the last device used wins. Keyboard/mouse input switches the prompts to keyboard/mouse
// glyphs (kbm_prompts), real controller input switches them back; see prompt_state.h.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DECLARE(bool, kbm);
REXCVAR_DECLARE(double, kbm_sensitivity);
REXCVAR_DECLARE(bool, kbm_invert_y);
REXCVAR_DECLARE(bool, kbm_prompts);

namespace bots {

using rex::X_RESULT;
using rex::X_STATUS;

class MouseKeyboardDriver final : public rex::input::InputDriver,
                                  public rex::ui::WindowInputListener,
                                  public rex::ui::WindowListener {
 public:
  MouseKeyboardDriver();
  ~MouseKeyboardDriver() override;

  // The live driver, or null. Read by the ProcessInput hook on the game thread.
  static MouseKeyboardDriver* instance() { return instance_.load(std::memory_order_acquire); }

  // Mouse motion since the last call, in pixels (+x right, +y up), scaled by kbm_sensitivity and
  // kbm_invert_y. Zero while the pointer isn't captured.
  void TakeMouseDelta(float& dx, float& dy);

  // The merged controller state the game read for user 0 (guest XInputGetState). Input beyond what this driver
  // contributed came from a real controller: prompts switch back to it.
  void NoteGuestPad(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry);

  // Menu mode: the game is paused (C1HUD NativePauseGame/NativeUnpauseGame) or the gameplay HUD stopped ticking
  // (title, front end, loading, cutscenes). The pointer is free and a left click is the engine's Enter key, which
  // the game's UI input aliases accept as confirm / "Press START" (Xe-C1Input.ini WidgetInputAliases).
  void SetPaused(bool paused) { paused_.store(paused, std::memory_order_relaxed); }
  void NoteHudTick() { hud_tick_.store(Now(), std::memory_order_relaxed); }
  bool MenuMode() const;
  // Engine Enter key edges (true = pressed) since the last call, oldest first. Game thread.
  void TakeEnterEvents(std::vector<bool>& out);

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE*) override;
  void OnWindowAvailable(rex::ui::Window* window) override;

  void OnKeyDown(rex::ui::KeyEvent& e) override { SetKey(uint16_t(e.virtual_key()), true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { SetKey(uint16_t(e.virtual_key()), false); }
  void OnMouseDown(rex::ui::MouseEvent& e) override { SetKey(ButtonKey(e), true); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { SetKey(ButtonKey(e), false); }
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

  void OnClosing(rex::ui::UIEvent&) override { Detach(); }
  void OnLostFocus(rex::ui::UISetupEvent&) override;
  void OnGotFocus(rex::ui::UISetupEvent&) override { has_focus_ = true; }

 private:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x4B424D00);  // 'KBM'
  // Pseudo virtual keys for the wheel, above the 0..255 range of real ones.
  static constexpr uint16_t kWheelUp = 256, kWheelDown = 257, kKeyCount = 258;

  static int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  static uint16_t ButtonKey(rex::ui::MouseEvent& e);
  bool Pressed(const std::string& keys) const;
  static bool Lists(const std::string& keys, uint16_t vk);
  void PushEnter(uint16_t vk, bool down);
  void SetKey(uint16_t vk, bool down);
  bool Enabled() const;
  void QueueCapture(bool want);
  void ApplyCaptureOnUIThread();
  void ReleaseCaptureOnUIThread(rex::ui::Window* window);
  void Detach();
  static void SetPromptMode(bool keyboard_mouse);

  static inline std::atomic<MouseKeyboardDriver*> instance_{nullptr};

  rex::ui::Window* window_ = nullptr;  // written on the UI thread only
  mutable std::mutex mutex_;
  bool key_down_[kKeyCount] = {};
  bool enter_down_[kKeyCount] = {};  // keys currently holding the engine's Enter down
  // Wheel notches are taps: the pad button stays down until this time.
  std::chrono::steady_clock::time_point wheel_until_[2] = {};
  float mouse_dx_ = 0, mouse_dy_ = 0;
  rex::input::X_INPUT_STATE last_out_{};  // what GetDeviceState last returned
  float moved_ = 0;                        // mouse travel toward switching the prompts, pixels
  std::vector<bool> enter_events_;         // engine Enter edges for the game thread
  bool click_is_enter_ = false;            // the left button went down in menu mode: its release is Enter's too
  std::atomic<bool> paused_{false};
  std::atomic<int64_t> hud_tick_{0};       // Now() of the last gameplay HUD tick
  bool last_menu_ = false;                 // GetDeviceState (guest thread), for the log
  uint32_t packet_ = 0;
  uint64_t last_sig_ = ~0ull;

  bool captured_ = false, relative_ = false;  // UI thread
  int32_t prev_x_ = 0, prev_y_ = 0;           // UI thread
  rex::ui::Window::CursorVisibility saved_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
  std::atomic<bool> has_focus_{true};
  std::atomic<bool> capture_wanted_{false}, capture_queued_{false};
};

}  // namespace bots
