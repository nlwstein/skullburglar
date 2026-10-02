// Native renderer shadow mode (P3): see shadow.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace rex::ui {
struct RawImage;
}

namespace bots::gfx {

// BOTS_GFX_SHADOW=<seconds>: how often a frame is compared; 0 when off or unavailable.
double ShadowInterval();
// BOTS_GFX_SHADOW_LIVE=1: every frame is rendered natively, by one renderer that keeps its state (memory tracked).
bool ShadowLive();
// Render thread, at the Swap that ends a recorded frame (before the original runs): renders the frame's PM4;
// `sample`: read the result back for ShadowCompare.
void ShadowRender(uint64_t frame, const std::vector<uint8_t>& pm4, bool sample);
// After the original Swap, once the presenter shows that frame: compares and logs.
void ShadowCompare(uint64_t frame, const rex::ui::RawImage& reference, const uint32_t* gamma_table);

}  // namespace bots::gfx
