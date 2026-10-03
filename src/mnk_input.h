// Keyboard and mouse (backlog B-104).
//
// Keys and mouse buttons drive a synthetic controller (merged into guest user 0, so a real pad works
// alongside): each pad control has a `kbm_*` cvar listing its keys. Mouse motion does NOT go through
// the right stick: the game's stick path applies C1PlayerInput's power curve and turn lag, which feels
// wrong for a mouse. Instead game_hooks.cpp drains TakeMouseDelta() each frame and feeds the engine's
// own PC MouseX / MouseY axes (linear, after the stick curve), as UE3 does on PC.
//
// The pointer is locked while the window has focus and no settings overlay is open (is_active()).
// The mouse wheel taps D-pad up/down (weapon select).
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

  static uint16_t ButtonKey(rex::ui::MouseEvent& e);
  bool Pressed(const std::string& keys) const;
  void SetKey(uint16_t vk, bool down);
  bool Enabled() const;
  void QueueCapture(bool want);
  void ApplyCaptureOnUIThread();
  void ReleaseCaptureOnUIThread(rex::ui::Window* window);
  void Detach();

  static inline std::atomic<MouseKeyboardDriver*> instance_{nullptr};

  rex::ui::Window* window_ = nullptr;  // written on the UI thread only
  mutable std::mutex mutex_;
  bool key_down_[kKeyCount] = {};
  // Wheel notches are taps: the pad button stays down until this time.
  std::chrono::steady_clock::time_point wheel_until_[2] = {};
  float mouse_dx_ = 0, mouse_dy_ = 0;
  uint32_t packet_ = 0;
  uint64_t last_sig_ = ~0ull;

  bool captured_ = false, relative_ = false;  // UI thread
  int32_t prev_x_ = 0, prev_y_ = 0;           // UI thread
  rex::ui::Window::CursorVisibility saved_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
  std::atomic<bool> has_focus_{true};
  std::atomic<bool> capture_wanted_{false}, capture_queued_{false};
};

}  // namespace bots
