// Scripted controller for unattended testing (backlog B-030).
//
// BOTS_INPUT="20:START,26:A,30:DOWN:300,31:A" — each step is <seconds>:<control>[:<hold ms>]
// (seconds since the driver was created ≈ process start; default hold 150 ms). Steps may
// overlap. Controls: A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT LT RT and the
// stick directions LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT (full deflection).
//
// BOTS_INPUT_RANDOM=<start sec>[:seed] adds random play after <start sec>: every 0.5–2 s a new
// left-stick direction, camera nudges, and occasional A/X/RT/LT/B/Y presses (soak testing).
// START/BACK are never pressed randomly (would open menus). Recovery: every 20 s the stick is centered and A
// pressed twice (death menu → reload); every 120 s the title → Continue Story path is replayed.
//
// It enumerates as a synthetic device, which SlotAssignment always merges into guest user 0, so a real
// controller connected at any time also lands on user 0 and both work. (As a physical pad it took user
// 0 itself and pushed a real controller to user 1, which the game ignores. Seen on Windows, 2026-10-01.)
#pragma once

#include <chrono>
#include <cstdlib>
#include <random>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/logging.h>

namespace bots {

using rex::X_RESULT;
using rex::X_STATUS;

class ScriptInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptInputDriver(const std::string& script)
      : InputDriver(nullptr, 0), start_(std::chrono::steady_clock::now()) {
    std::stringstream ss(script);
    std::string item;
    while (std::getline(ss, item, ',')) {
      Step s;
      std::stringstream is(item);
      std::string t, ctl, hold;
      std::getline(is, t, ':');
      std::getline(is, ctl, ':');
      std::getline(is, hold, ':');
      if (t.empty() || ctl.empty()) continue;
      s.at_ms = static_cast<int64_t>(std::atof(t.c_str()) * 1000);
      s.hold_ms = hold.empty() ? 150 : std::atoi(hold.c_str());
      s.control = ctl;
      steps_.push_back(s);
      REXLOG_INFO("script input: {} at {} ms for {} ms", s.control, s.at_ms, s.hold_ms);
    }
    if (const char* r = std::getenv("BOTS_INPUT_RANDOM"); r && *r) {
      random_from_ms_ = static_cast<int64_t>(std::atof(r) * 1000);
      const char* colon = std::strchr(r, ':');
      rng_.seed(colon ? std::strtoul(colon + 1, nullptr, 10) : 1);
      REXLOG_INFO("script input: random play from {} ms", random_from_ms_);
    }
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Script";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_SUCCESS;
    std::memset(out, 0, sizeof(*out));
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start_).count();
    uint16_t buttons = 0;
    for (auto& s : steps_) {
      if (now < s.at_ms || now >= s.at_ms + s.hold_ms) continue;
      Apply(s.control, buttons, *out);
      if (!s.logged) {
        s.logged = true;
        REXLOG_INFO("script input: press {} at {} ms", s.control, now);
      }
    }
    if (random_from_ms_ >= 0 && now >= random_from_ms_) Random(now, buttons, *out);
    out->gamepad.buttons = buttons;
    // Games detect change by packet number; bump it whenever the state changes.
    uint64_t sig = buttons | uint64_t(out->gamepad.left_trigger) << 16 |
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

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
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

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x53435249);  // 'SCRI'

  struct Step {
    int64_t at_ms = 0;
    int hold_ms = 150;
    std::string control;
    bool logged = false;
  };

  static void Apply(const std::string& c, uint16_t& b, rex::input::X_INPUT_STATE& st) {
    static const struct { const char* name; uint16_t mask; } kButtons[] = {
        {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
        {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040},   {"RS", 0x0080},
        {"LB", 0x0100},    {"RB", 0x0200},   {"A", 0x1000},    {"B", 0x2000},
        {"X", 0x4000},     {"Y", 0x8000},
    };
    for (auto& k : kButtons) {
      if (c == k.name) {
        b |= k.mask;
        return;
      }
    }
    constexpr int16_t kMax = 32767, kMin = -32768;
    if (c == "LT") st.gamepad.left_trigger = 0xFF;
    else if (c == "RT") st.gamepad.right_trigger = 0xFF;
    else if (c == "LUP") st.gamepad.thumb_ly = kMax;
    else if (c == "LDOWN") st.gamepad.thumb_ly = kMin;
    else if (c == "LLEFT") st.gamepad.thumb_lx = kMin;
    else if (c == "LRIGHT") st.gamepad.thumb_lx = kMax;
    else if (c == "RUP") st.gamepad.thumb_ry = kMax;
    else if (c == "RDOWN") st.gamepad.thumb_ry = kMin;
    else if (c == "RLEFT") st.gamepad.thumb_rx = kMin;
    else if (c == "RRIGHT") st.gamepad.thumb_rx = kMax;
  }

  // Random soak play: re-roll a "pose" (stick + optional button) at random intervals.
  void Random(int64_t now, uint16_t& b, rex::input::X_INPUT_STATE& st) {
    // Recovery, stick centered: every 120 s replay the title → Play Game → Continue Story path (if
    // random play quit to the title; in gameplay START pauses and A = "Resume Game", the rest is
    // harmless), and every 20 s otherwise press A twice (death menu: selected item + confirm —
    // the menu wraps, so the selection can't be reset; "Quit Game" is caught by the 120 s path).
    static const struct { int at_ms; const char* control; } kTitlePath[] = {
        {0, "START"}, {8000, "A"}, {14000, "DOWN"}, {16000, "A"}, {22000, "A"}, {28000, "A"}, {34000, "A"}};
    int64_t t = now - random_from_ms_;
    if (t % 120000 < 36000 && t >= 120000) {
      for (auto& k : kTitlePath)
        if (t % 120000 >= k.at_ms && t % 120000 < k.at_ms + 150) Apply(k.control, b, st);
      return;
    }
    int64_t phase = t % 20000;
    if (phase < 2500) {
      if ((phase >= 800 && phase < 950) || (phase >= 1800 && phase < 1950)) Apply("A", b, st);
      return;
    }
    if (now >= next_roll_ms_) {
      std::uniform_int_distribution<int> d(0, 99);
      std::uniform_int_distribution<int> ms(500, 2000);
      std::uniform_int_distribution<int> ax(-32767, 32767);
      lx_ = ax(rng_); ly_ = d(rng_) < 70 ? 32767 : ax(rng_);  // mostly forward
      rx_ = d(rng_) < 40 ? ax(rng_) / 2 : 0;
      static const char* kPress[] = {"A", "X", "B", "Y", "RT", "RT", "RT", "LT", "LB", "RB"};
      press_ = d(rng_) < 60 ? kPress[d(rng_) % 10] : "";
      next_roll_ms_ = now + ms(rng_);
      // Triggers are held for the whole roll (RT is also vehicle throttle, B-043); buttons tapped.
      press_until_ms_ = (*press_ == 'R' || *press_ == 'L') && press_[1] == 'T' ? next_roll_ms_ : now + 150;
      REXLOG_DEBUG("script input: random roll lx={} ly={} rx={} press={}", lx_, ly_, rx_, press_);
    }
    st.gamepad.thumb_lx = static_cast<int16_t>(lx_);
    st.gamepad.thumb_ly = static_cast<int16_t>(ly_);
    st.gamepad.thumb_rx = static_cast<int16_t>(rx_);
    if (*press_ && now < press_until_ms_) Apply(press_, b, st);
  }

  int64_t random_from_ms_ = -1, next_roll_ms_ = 0, press_until_ms_ = 0;
  int lx_ = 0, ly_ = 0, rx_ = 0;
  const char* press_ = "";
  std::mt19937 rng_;

  std::chrono::steady_clock::time_point start_;
  std::vector<Step> steps_;
  uint32_t packet_ = 0;
  uint64_t last_sig_ = ~0ull;
};

}  // namespace bots
