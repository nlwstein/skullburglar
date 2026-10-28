// The renderer couldn't start: a message box, then the process ends (native.cpp). Shared by the native renderer's own
// start-up and the new renderer (n1, hybrid_fill.cpp), which has no fallback: the player gets this instead.
#pragma once

#include <string>

namespace bots::gfx {

// Shows the failure with the log one click away (Open log / Quit), then exits. `gpu` and `reason` fill the message;
// `advice` replaces the usual driver advice when not empty. BOTS_NO_DIALOGS=1 skips the box (unattended runs).
[[noreturn]] void ShowStartFailure(const std::string& gpu, const std::string& reason, const std::string& advice = {});

}  // namespace bots::gfx
