// Pure rules for "Toggle aim" (B-372; no game or SDK types, so tests/aim_toggle_test.cpp can run it alone).
//
// With the setting off the left trigger (Aim) is a hold, as shipped. With it on, each press of Aim latches aiming on or
// off and letting go does nothing. It works on the controller state the game is about to read, so one rule covers a pad,
// the keyboard and mouse (any key bound to Aim, e.g. RMB and Shift together), and a mix.
#pragma once

#include <cstdint>

namespace bots::aim {

constexpr uint8_t kPressed = 64;  // the left trigger counts as pressed above this (a quarter pull)
constexpr uint8_t kFull = 255;

struct Toggle {
  bool prev_down = false;  // the trigger was pressed last frame
  bool prev_a = false;     // A (cover, run, use) was pressed last frame
  bool latched = false;    // aiming is latched on
};

// The left trigger value the game should see this frame.
//   enabled  the setting is on
//   allowed  gameplay on foot: false in menus, pause, loading, cutscenes and vehicles, where the trigger keeps its
//            normal meaning (menu input, brake) and a latch must not survive
//   lt       the trigger as read (0..255)
//   a_down   the A button is down: taking cover or running ends a latched aim, as sprinting ends aiming down sights
//            in other shooters
// A press while latched switches it off at once, even if the trigger is still held.
inline uint8_t Update(Toggle& t, bool enabled, bool allowed, uint8_t lt, bool a_down) {
  const bool down = lt > kPressed;
  const bool press = down && !t.prev_down;
  const bool a_press = a_down && !t.prev_a;
  t.prev_down = down;
  t.prev_a = a_down;
  if (!enabled || !allowed) {
    t.latched = false;
    return lt;
  }
  if (press) t.latched = !t.latched;
  else if (a_press) t.latched = false;
  return t.latched ? kFull : 0;
}

}  // namespace bots::aim
