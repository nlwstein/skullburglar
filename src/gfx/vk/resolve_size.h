// Pure rule: which colour resolves the pre-1.0 renderer keeps at guest size under a render scale (tests/resolve_size_test.cpp).
#pragma once

#include <cstdint>

namespace bots::gfx::vk {

// A colour resolve into a quarter-resolution buffer (UE3's blur chain, 352x182) is kept at guest size when the render
// scale is above 1 (Renderer::SmallResolve). Below 256 wide are the tiny tracking textures n1's interop finds by host
// size (damage 128x64, god rays 128x128); half-resolution buffers and up stay scaled. 1280x720 is the guest frame.
inline constexpr bool SmallResolveSize(uint32_t width, uint32_t height) {
  return width >= 256 && width <= 512 && height >= 128 && width * height * 6 <= 1280u * 720u;
}

}  // namespace bots::gfx::vk
