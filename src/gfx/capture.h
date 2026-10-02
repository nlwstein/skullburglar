// D3D frame capture (native renderer, P1): see capture.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace bots::gfx {

// True when BOTS_GFX_CAPTURE is set: the D3D hooks then call CapturePre/CapturePost around every entry point.
bool CaptureConfigured();

// Before the original runs. Returns a token for CapturePost (-1: nothing recorded).
int64_t CapturePre(uint32_t entry, PPCContext& ctx);
// After the original returned (ctx holds its results).
void CapturePost(uint32_t entry, int64_t token, PPCContext& ctx);

}  // namespace bots::gfx
