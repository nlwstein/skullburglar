// B-318 (#24): the focus highlight on the object the player is aiming at.
//
// The game sets `ObjectHasFocus` on the targeted actor, and the engine then sets the scalar parameter `Activated` on the
// object's shared material instance (0 -> 1): the material graph desaturates the diffuse and adds a pulsing lime glow.
// The cooked material never carries that parameter's live value, so the scene feed marks the instance and n1 draws it
// with `Activated` = 1. The property system describes a bool as a word offset plus a one-bit mask; the feed reads the
// word from guest memory (host order). Pure functions, tested in tests/focus_flag_test.cpp.
#pragma once

#include <cstdint>

namespace bots::focus {

// Bit 0 of HybridScene::Instance::flags / N1Instance::flags (scenekit INST_FOCUS): draw with Activated = 1.
inline constexpr uint32_t kInstFocus = 1;

// The flags of one static instance from the word holding the owner's ObjectHasFocus. A property that was not found
// (mask 0) or a switch that is off (BOTS_N1_FOCUS=0) gives nothing, so a missing field can't light every object.
constexpr uint32_t InstFlags(bool enabled, uint32_t word, uint32_t mask) {
  return enabled && mask != 0 && (word & mask) != 0 ? kInstFocus : 0;
}

// BOTS_N1_FOCUS: on unless set to "0".
constexpr bool EnabledBy(const char* value) { return !(value && value[0] == '0' && value[1] == 0); }

}  // namespace bots::focus
