// The pre-1.0 renderer's visibility test (B-333): the launcher's "Pre-1.0 renderer: hide objects behind walls" setting
// (bots.toml key `pre1_occlusion`, also `--pre1_occlusion=off`). The renderer itself reads BOTS_GFX_OCCLUSION (gfx/native.cpp),
// so the setting becomes that variable for the game process; a BOTS_GFX_OCCLUSION already in the environment is the
// player's own and wins. Plain C++ so tests/occlusion_choice_test.cpp can run it alone.
#pragma once

#include <string_view>

namespace bots::setup {

// The BOTS_GFX_OCCLUSION value for a setting value: "0" (off), "1" (on), or nullptr (automatic: the renderer decides by
// GPU; anything unrecognized, like a typo in bots.toml, is automatic too).
inline const char* Pre1OcclusionEnv(std::string_view setting) {
  if (setting == "off") return "0";
  if (setting == "on") return "1";
  return nullptr;
}

}  // namespace bots::setup
