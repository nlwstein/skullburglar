// Plain-assert tests for src/counterkill_port.h. Run: c++ -std=c++20 -I src tests/counterkill_port_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "counterkill_port.h"

using namespace bots::counterkill;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  // The chain's materials during a stylized counter-kill (n1ffi's post-material log, 42285 alcove, Low_01 / Low_04).
  CHECK(IsKillPostMaterial("FX_MerkYouSpaceRescource.BKGAndGhost_MIC"));
  CHECK(IsKillPostMaterial("FX_MerkYouSpaceRescource.FadeIn_MAT_MIC"));
  CHECK(IsKillPostMaterial("FX_MerkYouSpaceRescource.BurnOut_MIC"));
  CHECK(IsKillPostMaterial("FX_MerkYouSpaceRescource.PosterAndGhost_MIC"));
  // Everything else the chain holds in play: level grades, Gangsta Fire, death, damage, scope, pause.
  CHECK(!IsKillPostMaterial("MIS_TheFlyoverMisc.FlyoverPP_Final_MAT01"));
  CHECK(!IsKillPostMaterial("C1HUD.SlowmoEffect_MatInst"));
  CHECK(!IsKillPostMaterial("UI_DeathEffect.FaderDeath_MIC"));
  CHECK(!IsKillPostMaterial("FX_Damage.DamageEffectMaterial_MIC"));
  CHECK(!IsKillPostMaterial("UI_IngameMenus.PauseMAT"));
  // The package name alone, another package with the same start, a different case, empty.
  CHECK(!IsKillPostMaterial("FX_MerkYouSpaceRescource."));
  CHECK(!IsKillPostMaterial("FX_MerkYouSpace.WhiteRoom_RTT"));
  CHECK(!IsKillPostMaterial("fx_merkyouspacerescource.BKGAndGhost_MIC"));
  CHECK(!IsKillPostMaterial(""));
  // Only n1's frames (suppressed, fill 4) of a kill, with the switch on.
  CHECK(GameDrawsScene(2, 4, true, true));
  CHECK(GameDrawsScene(1, 4, true, true));
  CHECK(!GameDrawsScene(2, 4, false, true));
  CHECK(!GameDrawsScene(2, 4, true, false));  // BOTS_N1_COUNTERKILL_PORT=0
  CHECK(!GameDrawsScene(0, 4, true, true));   // already the game's scene (dual render, pre-1.0, n1 not ready)
  CHECK(!GameDrawsScene(2, 3, true, true));   // debug grid fills
  CHECK(!GameDrawsScene(2, 0, true, true));
  // B-345: the switch.
  CHECK(ParseMode(nullptr, nullptr) == Mode::kPort);
  CHECK(ParseMode("native", nullptr) == Mode::kNative);
  CHECK(ParseMode("Native", "0") == Mode::kNative);  // the new variable wins
  CHECK(ParseMode("off", nullptr) == Mode::kOff);
  CHECK(ParseMode("0", nullptr) == Mode::kOff);
  CHECK(ParseMode("port", "0") == Mode::kPort);
  CHECK(ParseMode("", "0") == Mode::kOff);            // empty = not set
  CHECK(ParseMode(nullptr, "0") == Mode::kOff);       // 1.9.1's BOTS_N1_COUNTERKILL_PORT=0
  CHECK(ParseMode(nullptr, "1") == Mode::kPort);
  CHECK(ParseMode(nullptr, "00") == Mode::kPort);
  CHECK(NativePorted("FX_MerkYouSpaceRescource.BKGAndGhost_MIC"));
  CHECK(NativePorted("FX_MerkYouSpaceRescource.FadeIn_MAT_MIC"));
  CHECK(NativePorted("FX_MerkYouSpaceRescource.BurnOut_MIC"));
  CHECK(!NativePorted("FX_MerkYouSpaceRescource.PosterAndGhost_MIC"));
  CHECK(KillToGame(Mode::kPort, true, true));
  CHECK(!KillToGame(Mode::kPort, false, true));
  CHECK(!KillToGame(Mode::kOff, true, false));
  CHECK(!KillToGame(Mode::kNative, true, true));
  CHECK(KillToGame(Mode::kNative, true, false));     // an unported kill material: the game draws it
  CHECK(KillCull(Mode::kNative, true, false));
  CHECK(!KillCull(Mode::kNative, true, true));
  CHECK(!KillCull(Mode::kPort, true, false));
  CHECK(!KillCull(Mode::kNative, false, false));
  CHECK(KeepInKill(52.5f, 52.5f));
  CHECK(KeepInKill(52.4995f, 52.5f));
  CHECK(!KeepInKill(52.45f, 52.5f));  // last drawn three frames earlier at 60 fps: culled by the kill's view
  CHECK(!KeepInKill(0.0f, 52.5f));
  if (failures) return 1;
  std::printf("counterkill_port_test: ok\n");
  return 0;
}
