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

// B-345: who draws a stylized counter-kill in n1 mode. BOTS_N1_COUNTERKILL=port (default, 1.9.1): the game's renderer;
// native: n1, with the effect's post materials ported (n1ffi postmat.rs), its lights and the game's culling; off: n1
// as in 1.9.0 (#32's look). BOTS_N1_COUNTERKILL_PORT, 1.9.1's switch, still works: 0 = off, anything else = port.
// BOTS_N1_COUNTERKILL wins when both are set.
enum class Mode { kOff, kPort, kNative };

constexpr bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// `counterkill` and `legacy_port` are the two variables' values (nullptr = not set).
constexpr Mode ParseMode(const char* counterkill, const char* legacy_port) {
  if (counterkill && *counterkill) {
    const std::string_view v(counterkill);
    if (EqualsNoCase(v, "native")) return Mode::kNative;
    if (EqualsNoCase(v, "off") || v == "0") return Mode::kOff;
    return Mode::kPort;
  }
  if (legacy_port && *legacy_port == '0' && legacy_port[1] == 0) return Mode::kOff;
  return Mode::kPort;
}

// The kill's post materials n1 has a pass for (n1ffi postmat.rs Kind::MerkBkg / MerkFadeIn / MerkBurnOut). Any other
// FX_MerkYouSpaceRescource material (PosterAndGhost, not seen in play) makes a native frame go to the game instead.
constexpr bool NativePorted(std::string_view path) {
  for (std::string_view p : {"FX_MerkYouSpaceRescource.BKGAndGhost_MIC", "FX_MerkYouSpaceRescource.BKGAndGhost_MAT",
                             "FX_MerkYouSpaceRescource.FadeIn_MAT_MIC", "FX_MerkYouSpaceRescource.FadeIn_MAT",
                             "FX_MerkYouSpaceRescource.BurnOut_MIC", "FX_MerkYouSpaceRescource.BurnOut_MAT"})
    if (path == p) return true;
  return false;
}

// Whether a kill frame (`kill`: a FX_MerkYouSpaceRescource material in the chain) is handed to the game's renderer.
// `all_ported`: every such material in the chain is NativePorted.
constexpr bool KillToGame(Mode mode, bool kill, bool all_ported) {
  if (!kill || mode == Mode::kOff) return false;
  return mode == Mode::kPort || !all_ported;
}

// Whether n1 culls the level to the game's own view of the kill (native mode, a kill n1 draws).
constexpr bool KillCull(Mode mode, bool kill, bool to_game) { return mode == Mode::kNative && kill && !to_game; }

// The kill's culling: during it the game's view draws only the white room, the fighters and the kill's particles, so
// only their LastRenderTime keeps up with the frames (the game's visibility pass runs in n1's frames too, hybrid.cpp
// mode 2). A primitive is kept when it was rendered within `slack` seconds of the newest frame any primitive was
// (`newest`, the walk's maximum LastRenderTime). The feed walks on the game thread while the render thread's visibility
// pass may be half way through the previous frame, so a visible primitive can be one frame behind the newest; with a
// 1 ms slack the room and the fighters dropped out of about half the frames (merk-native vn1). 0.15 s covers a frame
// at 7 fps; the level, culled from the kill's first frame, is gone after that (under the skull wipe).
constexpr float kKillCullSlack = 0.15f;
constexpr bool KeepInKill(float last_render_time, float newest, float slack = kKillCullSlack) {
  return last_render_time >= newest - slack;
}

// Whether a frame n1 would draw (the game's scene suppressed, n1's fill 4) goes to the game's renderer instead:
// only with the switch on and only for a stylized counter-kill's frame.
constexpr bool GameDrawsScene(int suppress, uint32_t fill, bool kill_frame, bool switch_on) {
  return switch_on && suppress != 0 && fill == 4 && kill_frame;
}

}  // namespace bots::counterkill
