// Controller navigation for the launcher window (setup_dialog.h). The window opens before the runtime and its input
// system exist, so it opens the controllers itself through SDL and feeds them to ImGui as gamepad keys: A presses,
// B backs out of a list, the d-pad and left stick move between controls. The app's SDL loop already pumps events
// every frame while the window is open, so this only reads state. It lives only as long as the window: Play tears
// it down before the game starts, and the game's own SDL input driver opens the controllers again.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/logging.h>

namespace bots::setup {

class PadNav {
 public:
  PadNav() {
    ok_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!ok_) REXLOG_WARN("setup: no controller support in the launcher: {}", SDL_GetError());
  }

  ~PadNav() {
    for (auto& p : pads_) SDL_CloseGamepad(p.pad);
    if (io_) {
      io_->ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
      io_->BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
      ClearKeys(*io_);
    }
    if (ok_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }

  PadNav(const PadNav&) = delete;
  PadNav& operator=(const PadNav&) = delete;

  // Once per frame, before the window draws.
  void Update(ImGuiIO& io) {
    if (!ok_) return;
    io_ = &io;
    SDL_UpdateGamepads();
    Refresh();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    if (pads_.empty()) {
      io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
      return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // Every controller drives the window: a button is down when it is down on any of them.
    for (const auto& [button, key] : kButtons) {
      bool down = false;
      for (const auto& p : pads_) down |= SDL_GetGamepadButton(p.pad, button);
      io.AddKeyEvent(key, down);
    }
    for (const auto& a : kAxes) {
      float v = 0.0f;
      for (const auto& p : pads_) v = std::max(v, Analog(SDL_GetGamepadAxis(p.pad, a.axis), a.sign, a.dead));
      io.AddKeyAnalogEvent(a.key, v > 0.1f, v);
    }
  }

  bool Connected() const { return !pads_.empty(); }

  // A PlayStation pad is connected (the window names its buttons: Cross, Circle, Options).
  bool PlayStation() const {
    return std::any_of(pads_.begin(), pads_.end(), [](const Pad& p) {
      const SDL_GamepadType t = SDL_GetGamepadType(p.pad);
      return t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
    });
  }

 private:
  struct Pad {
    SDL_JoystickID id;
    SDL_Gamepad* pad;
  };
  struct Axis {
    SDL_GamepadAxis axis;
    int sign;     // which half of the axis
    int dead;     // values within this count as 0
    ImGuiKey key;
  };

  static constexpr std::pair<SDL_GamepadButton, ImGuiKey> kButtons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, ImGuiKey_GamepadFaceDown},
      {SDL_GAMEPAD_BUTTON_EAST, ImGuiKey_GamepadFaceRight},
      {SDL_GAMEPAD_BUTTON_WEST, ImGuiKey_GamepadFaceLeft},
      {SDL_GAMEPAD_BUTTON_NORTH, ImGuiKey_GamepadFaceUp},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
      {SDL_GAMEPAD_BUTTON_START, ImGuiKey_GamepadStart},
      {SDL_GAMEPAD_BUTTON_BACK, ImGuiKey_GamepadBack},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ImGuiKey_GamepadL1},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ImGuiKey_GamepadR1},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, ImGuiKey_GamepadL3},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, ImGuiKey_GamepadR3},
  };
  // Stick dead zone as in ImGui's own SDL backend; triggers have almost none.
  static constexpr int kStickDead = 8000, kTriggerDead = 1000;
  static constexpr Axis kAxes[] = {
      {SDL_GAMEPAD_AXIS_LEFTX, -1, kStickDead, ImGuiKey_GamepadLStickLeft},
      {SDL_GAMEPAD_AXIS_LEFTX, +1, kStickDead, ImGuiKey_GamepadLStickRight},
      {SDL_GAMEPAD_AXIS_LEFTY, -1, kStickDead, ImGuiKey_GamepadLStickUp},
      {SDL_GAMEPAD_AXIS_LEFTY, +1, kStickDead, ImGuiKey_GamepadLStickDown},
      {SDL_GAMEPAD_AXIS_RIGHTX, -1, kStickDead, ImGuiKey_GamepadRStickLeft},
      {SDL_GAMEPAD_AXIS_RIGHTX, +1, kStickDead, ImGuiKey_GamepadRStickRight},
      {SDL_GAMEPAD_AXIS_RIGHTY, -1, kStickDead, ImGuiKey_GamepadRStickUp},
      {SDL_GAMEPAD_AXIS_RIGHTY, +1, kStickDead, ImGuiKey_GamepadRStickDown},
      {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, +1, kTriggerDead, ImGuiKey_GamepadL2},
      {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, +1, kTriggerDead, ImGuiKey_GamepadR2},
  };

  static float Analog(Sint16 raw, int sign, int dead) {
    const float v = float(raw) * float(sign);
    return std::clamp((v - float(dead)) / (32767.0f - float(dead)), 0.0f, 1.0f);
  }

  // Opens controllers that were plugged in and closes the ones that went away.
  void Refresh() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::vector<SDL_JoystickID> now(ids, ids + (ids ? count : 0));
    SDL_free(ids);
    for (auto it = pads_.begin(); it != pads_.end();) {
      if (std::find(now.begin(), now.end(), it->id) != now.end()) {
        ++it;
        continue;
      }
      REXLOG_INFO("setup: controller {} disconnected", it->id);
      SDL_CloseGamepad(it->pad);
      it = pads_.erase(it);
    }
    for (SDL_JoystickID id : now) {
      if (std::any_of(pads_.begin(), pads_.end(), [id](const Pad& p) { return p.id == id; })) continue;
      if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
        const char* name = SDL_GetGamepadName(pad);
        REXLOG_INFO("setup: controller {} connected: {}", id, name ? name : "?");
        pads_.push_back({id, pad});
      }
    }
  }

  static void ClearKeys(ImGuiIO& io) {
    for (const auto& [button, key] : kButtons) io.AddKeyEvent(key, false);
    for (const auto& a : kAxes) io.AddKeyAnalogEvent(a.key, false, 0.0f);
  }

  bool ok_ = false;
  ImGuiIO* io_ = nullptr;
  std::vector<Pad> pads_;
};

}  // namespace bots::setup
