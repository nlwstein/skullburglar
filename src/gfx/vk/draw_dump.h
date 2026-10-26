// Scene-feed draw dump (research spike): with BOTS_SCENEFEED_DIR set, the game's "scenefeed draws <n>" command writes
// <dir>/draws.req and the renderer then dumps its next n frames of draws to <dir>/draws_<host_us>.bin, one record per
// recorded draw: pass info plus all 256 vertex-shader float constants, so draws can be matched offline to the
// primitives in the scene-feed JSON (UE3 uploads each mesh's LocalToWorld as vertex-shader constants).
//   file: "DFR1", u32 version 1, i64 steady-clock microseconds at the frame's first draw, then records:
//   record: "DRW1", u32 draw index in the frame, u64 VS hash, u64 PS hash (0: none), u32 pass width, u32 pass height,
//           u32 flags (1 depth-only, 2 multisampled, 4 rectangle list, 8 indexed), u32 Xenos primitive type,
//           u32 vertex/index count, u32 0, then 1024 floats (c0.xyzw .. c255.xyzw); all little-endian.
#pragma once

#include <cstdint>

namespace bots::gfx::drawdump {
bool Active();  // a frame is being dumped
void Record(const uint32_t* vs_constants, uint64_t vs, uint64_t ps, uint32_t width, uint32_t height, uint32_t flags,
            uint32_t prim, uint32_t count);
void OnPresent();  // frame boundary: finish the frame being dumped, pick up a new request
}  // namespace bots::gfx::drawdump
