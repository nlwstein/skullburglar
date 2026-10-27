// Scene-feed draw dump (research spike): with BOTS_SCENEFEED_DIR set, the game's "scenefeed draws <n>" command writes
// <dir>/draws.req and the renderer then dumps its next n frames of draws to <dir>/draws_<host_us>.bin, one record per
// recorded draw: pass info plus all 256 vertex-shader float constants, so draws can be matched offline to the
// primitives in the scene-feed JSON (UE3 uploads each mesh's LocalToWorld as vertex-shader constants).
//   file: "DFR1", u32 version 2, i64 steady-clock microseconds at the frame's first draw, then records:
//   record: "DRW1", u32 draw index in the frame, u64 VS hash, u64 PS hash (0: none), u32 pass width, u32 pass height,
//           u32 flags (1 depth-only, 2 multisampled, 4 rectangle list, 8 indexed), u32 Xenos primitive type,
//           u32 vertex/index count, u32 stream count, then 1024 floats (c0.xyzw .. c255.xyzw), then per vertex stream:
//           u32 fetch constant, u32 guest address, u32 stride bytes, u32 Xenos endian swap, u32 attribute count,
//           u32 data bytes, attribute count x {u32 offset in words, u32 Xenos vertex
//           format}, then the stream's data as it is in guest memory (at most kMaxStreamBytes). Headers little-endian.
//   version 4 adds, after the streams: "PSC1", u32 RB_BLENDCONTROL0, u32 RB_DEPTHCONTROL, u32 RB_STENCILREFMASK, then
//           1024 floats (pixel shader constants c0.xyzw .. c255.xyzw, register file 256..511). Each shader's microcode
//           disassembly is written once per hash next to the dump as <dir>/shaders/<hash>.{vs,ps}.txt.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bots::gfx::drawdump {
constexpr uint32_t kMaxStreamBytes = 64 * 1024;
struct Stream {
  struct Attribute {
    uint32_t offset_words, format;
  };
  uint32_t fetch_constant = 0, address = 0, stride = 0, endian = 0, size = 0;
  const uint8_t* data = nullptr;
  std::vector<Attribute> attributes;
};
bool Active();  // a frame is being dumped
struct PixelState {
  const uint32_t* ps_constants = nullptr;  // 1024 words, or null
  uint32_t blend0 = 0, depth = 0, stencil = 0;
};
void Record(const uint32_t* vs_constants, uint64_t vs, uint64_t ps, uint32_t width, uint32_t height, uint32_t flags,
            uint32_t prim, uint32_t count, const std::vector<Stream>& streams, const PixelState& pixel);
// Writes <dir>/shaders/<hash>.<kind>.txt once per hash (kind "vs" or "ps"); `text` is only called the first time.
bool WantShader(uint64_t hash, const char* kind);
void WriteShader(uint64_t hash, const char* kind, const std::string& text);
void OnPresent();  // frame boundary: finish the frame being dumped, pick up a new request
}  // namespace bots::gfx::drawdump
