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
  if (failures) return 1;
  std::printf("counterkill_port_test: ok\n");
  return 0;
}
