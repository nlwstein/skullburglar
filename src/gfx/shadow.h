// Native renderer shadow mode (P3): see shadow.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace rex::ui {
struct RawImage;
}

namespace bots::gfx {

// BOTS_GFX_SHADOW=<seconds>: how often a frame is rendered natively and compared; 0 when off or unavailable.
double ShadowInterval();
// Render thread, at the Swap that ends a sampled frame (before the original runs): renders the frame's PM4.
void ShadowRender(uint64_t frame, const std::vector<uint8_t>& pm4);
// After the original Swap, once the presenter shows that frame: compares and logs.
void ShadowCompare(uint64_t frame, const rex::ui::RawImage& reference, const uint32_t* gamma_table);

}  // namespace bots::gfx
