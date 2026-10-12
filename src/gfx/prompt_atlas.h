// Keyboard/mouse (backlog B-105) and PlayStation (B-160) button prompts.
//
// Every controller-button prompt in the game (menu button bar, HUD action prompts, pickups, tutorials) cuts its glyph
// out of one texture, UI_Shared.360_buttons_COMP01. While keyboard/mouse or a PlayStation pad is the last device used
// (prompt_state.h), the renderer loads a replacement for it: the same cell layout with a keyboard or mouse glyph for
// the first key bound to each button (kbm_* cvars), or the DualShock 4 / DualSense button for each Xbox one.
// Glyphs: Kenney "Input Prompts" (CC0, assets/prompts).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../prompt_state.h"

namespace bots::prompts {

// UI_Shared.360_buttons_COMP01 in guest memory: 256x128 DXT5, tiled, one level; FNV-1a 64 of its 32 KiB.
inline constexpr uint32_t kButtonAtlasWidth = 256, kButtonAtlasHeight = 128, kButtonAtlasBytes = 32768;
inline constexpr uint64_t kButtonAtlasHash = 0xEC096A040B51742Aull;

inline uint64_t Hash(const uint8_t* data, size_t size) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
  return h;
}

// The atlas for a device other than kXbox, RGBA8 rows top to bottom (kButtonAtlasWidth x kButtonAtlasHeight); the
// keyboard/mouse one from the current bindings. Empty if the glyph sheet failed to load.
std::vector<uint8_t> BuildButtonAtlas(PromptDevice device);

}  // namespace bots::prompts
