// Pure rule of the pre-1.0 renderer's depth bias for decal draws (tests/depth_bias_test.cpp). No renderer or SDK
// types: draw.cpp calls it.
#pragma once

#include <algorithm>

namespace bots::gfx::vk {

// What the game's depth offset becomes on the host. A draw the game offsets at all (UE3 draws decal pieces with a
// +2e-5 polygon offset) is a decal piece or similar: it lies on a surface drawn earlier, so it also gets a slope-scaled
// bias, which covers the depth difference that grows with the surface's slope (grazing roads, long walls).
struct DepthBias {
  float constant;  // Vulkan depthBiasConstantFactor
  float slope;     // Vulkan depthBiasSlopeFactor
};

// Defaults for decal draws (draw.cpp): the constant is the game's bias times this, the slope bias is this many times
// the larger depth gradient per pixel. Overridden by BOTS_GFX_DEPTH_BIAS_SCALE and BOTS_GFX_DECAL_SLOPE.
// The game's own bias is a constant 2e-5 with no slope (every decal in Mission 2 reads DepthBias -2e-5, SlopeScaleDepthBias 0).
// Decal pieces are clipped triangles of the receiver, so on a grazing road their depth differs from the road's by an amount
// that grows with the surface's depth gradient: the constant (x16 in 1.17) leaves slivers and whole streaks of scorch marks and
// street trash missing there, on a GTX 1070 as on the reporter's RTX 4060. Measured on the 1070 (Mission 2 street, and the wall
// and floor decals of three other chapters): slope 4 restores nearly all of it and x32 adds margin; the cost is at most 66 changed
// pixels of a 720p frame in the wall poses (no decal showing through walls); slope 6 and x64 start to show through (200+ pixels).
inline constexpr float kDecalBiasScale = 32.0f;
inline constexpr float kDecalSlope = 4.0f;

// guest_constant: the game's offset after the format's factor; guest_slope: its slope factor times the guest's
// subpixel unit (what Vulkan wants). constant_scale multiplies the constant of offset draws only (was every draw, but
// only offset draws have a non-zero constant). decal_slope 0: off. Negative: a test that pushes decals back.
inline DepthBias HostDepthBias(float guest_constant, float guest_slope, float constant_scale, float decal_slope) {
  const bool offset_draw = guest_constant != 0.0f;
  DepthBias bias{guest_constant * constant_scale, guest_slope};
  if (offset_draw && decal_slope != 0.0f) bias.slope = decal_slope < 0.0f ? decal_slope : std::max(guest_slope, decal_slope);
  return bias;
}

}  // namespace bots::gfx::vk
