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
// 16: the value that removed most of #36's flicker on an RTX 4060; on a GTX 1070 anything up to 64 changes under 60 pixels
// of a 720p frame and 256 starts to show (decals through thin geometry). Slope 1: a 16x margin under where bleed shows.
inline constexpr float kDecalBiasScale = 16.0f;
inline constexpr float kDecalSlope = 1.0f;

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
