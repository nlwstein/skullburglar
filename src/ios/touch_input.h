// The on-screen touch controller (ios_app.mm) as a synthetic pad: synthetic devices drive player one
// alongside any real pad, so it reports a neutral state while hidden.
#pragma once

#include <cstring>
#include <vector>

#include <rex/input/input_driver.h>

#include "ios/ios_app.h"

namespace bots::ios {

class TouchInputDriver final : public rex::input::InputDriver {
 public:
  TouchInputDriver() : InputDriver(nullptr, 0) {}

  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Touch controller";
    info.synthetic = true;
    out.push_back(info);
  }

  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_SUCCESS;
    std::memset(out, 0, sizeof(*out));
    auto& pad = TouchPad();
    if (pad.visible.load(std::memory_order_relaxed)) {
      out->gamepad.buttons = pad.buttons.load(std::memory_order_relaxed);
      out->gamepad.left_trigger = pad.left_trigger.load(std::memory_order_relaxed);
      out->gamepad.right_trigger = pad.right_trigger.load(std::memory_order_relaxed);
      out->gamepad.thumb_lx = pad.thumb_lx.load(std::memory_order_relaxed);
      out->gamepad.thumb_ly = pad.thumb_ly.load(std::memory_order_relaxed);
      out->gamepad.thumb_rx = pad.thumb_rx.load(std::memory_order_relaxed);
      out->gamepad.thumb_ry = pad.thumb_ry.load(std::memory_order_relaxed);
    }
    // Games detect change by packet number; bump it whenever the state changes.
    uint64_t sig = out->gamepad.buttons | uint64_t(out->gamepad.left_trigger) << 16 |
                   uint64_t(out->gamepad.right_trigger) << 24 |
                   uint64_t(uint16_t(out->gamepad.thumb_lx)) << 32 |
                   uint64_t(uint16_t(out->gamepad.thumb_ly)) << 48;
    sig ^= (uint64_t(uint16_t(out->gamepad.thumb_rx)) << 7) ^
           (uint64_t(uint16_t(out->gamepad.thumb_ry)) << 29);
    if (sig != last_sig_) {
      last_sig_ = sig;
      ++packet_;
    }
    out->packet_number = packet_;
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                      rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                   rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x544F5543);  // 'TOUC'
  uint64_t last_sig_ = 0;
  uint32_t packet_ = 0;
};

}  // namespace bots::ios
