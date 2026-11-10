// B-323 (GitHub #32): which frames of a stylized counter-kill the game draws itself in n1 mode.
//
// The stylized counter-kills (skull wipe, parchment backdrop) are SfCore's "Merk You Space" effect: a white room mesh
// spawned around the fighters, lights for them, the level culled from the game's own view, and a post chain whose
// MaterialEffects (FX_MerkYouSpaceRescource.BKGAndGhost_MIC, FadeIn_MAT_MIC, BurnOut_MIC, PosterAndGhost_MIC) paint
// the parchment from the scene colour. n1 draws none of that, so while one of those materials is in the player's post
// chain the game's own renderer draws the scene (scene_feed.cpp UpdateStylizedKill, hybrid.cpp Render).
// Pure functions, tested in tests/counterkill_port_test.cpp.
#pragma once

#include <cstdint>
#include <string_view>

namespace bots::counterkill {

// The package of every material the effect puts in the post chain (case as the game names it).
inline constexpr std::string_view kPostPackage = "FX_MerkYouSpaceRescource.";

// Whether a post-chain material (its object path) belongs to the stylized counter-kill.
constexpr bool IsKillPostMaterial(std::string_view path) {
  return path.size() > kPostPackage.size() && path.substr(0, kPostPackage.size()) == kPostPackage;
}

// Whether a frame n1 would draw (the game's scene suppressed, n1's fill 4) goes to the game's renderer instead:
// only with the switch on and only for a stylized counter-kill's frame.
constexpr bool GameDrawsScene(int suppress, uint32_t fill, bool kill_frame, bool switch_on) {
  return switch_on && suppress != 0 && fill == 4 && kill_frame;
}

}  // namespace bots::counterkill
