// Plain-assert tests for src/gfx/vk/depth_bias.h. Run: c++ -std=c++20 -I src tests/depth_bias_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "gfx/vk/depth_bias.h"

using namespace bots::gfx::vk;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  // A draw the game does not offset keeps its (zero) constant and slope, whatever the knobs say.
  CHECK(HostDepthBias(0.0f, 0.0f, 16.0f, 1.0f).constant == 0.0f);
  CHECK(HostDepthBias(0.0f, 0.0f, 16.0f, 1.0f).slope == 0.0f);
  CHECK(HostDepthBias(0.0f, 0.25f, 1.0f, 1.0f).slope == 0.25f);
  // A decal piece (guest constant 335.5, no slope): the constant scales, the slope becomes the decal slope.
  CHECK(HostDepthBias(335.5f, 0.0f, 1.0f, 1.0f).constant == 335.5f);
  CHECK(HostDepthBias(335.5f, 0.0f, kDecalBiasScale, kDecalSlope).constant == 335.5f * 16.0f);
  CHECK(HostDepthBias(335.5f, 0.0f, 16.0f, 1.0f).constant == 335.5f * 16.0f);
  CHECK(HostDepthBias(335.5f, 0.0f, 1.0f, 1.0f).slope == 1.0f);
  // A game slope above the decal slope wins; below it, the decal slope.
  CHECK(HostDepthBias(335.5f, 3.0f, 1.0f, 1.0f).slope == 3.0f);
  CHECK(HostDepthBias(335.5f, 0.5f, 1.0f, 1.0f).slope == 1.0f);
  // Off: decal slope 0 leaves the game's slope alone.
  CHECK(HostDepthBias(335.5f, 0.5f, 1.0f, 0.0f).slope == 0.5f);
  CHECK(HostDepthBias(335.5f, 0.0f, 1.0f, 0.0f).slope == 0.0f);
  // A negative decal slope is a test and replaces the slope outright (pushes the decal back).
  CHECK(HostDepthBias(335.5f, 0.5f, 1.0f, -0.5f).slope == -0.5f);
  CHECK(HostDepthBias(0.0f, 0.5f, 1.0f, -0.5f).slope == 0.5f);
  // Constant scale 0 switches the constant off.
  CHECK(HostDepthBias(335.5f, 0.0f, 0.0f, 1.0f).constant == 0.0f);
  // The defaults are what draw.cpp ships (kept here so a change is deliberate).
  static_assert(kDecalSlope == 1.0f);
  static_assert(kDecalBiasScale == 16.0f);

  if (failures) return 1;
  std::printf("depth_bias_test: ok\n");
  return 0;
}
