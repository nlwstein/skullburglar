// Which renderer draws the game: the new one (n1, the default) or the pre-1.0 native renderer (deprecated).
// The player's choice is the `renderer` setting (bots.toml, launcher, --renderer=): "new" or "pre1". Only the exact
// value "pre1" selects the deprecated renderer (also the default on tvOS: kDefaultRenderer), so every existing config (a `gpu_plugin = "native"` written by an older
// launcher, a missing key, a typo) lands on the new one. `gpu_plugin` stays the SDK's GPU backend; the launcher no
// longer writes it. The new renderer is the native Vulkan renderer with n1 drawing the 3D scene ("hybrid fill 4",
// src/hybrid.cpp and src/gfx/vk/hybrid_fill.cpp), so it needs the native backend and a build with n1 linked in.
// The BOTS_HYBRID_* and BOTS_N1_* environment variables of the test launchers still override every default below.
#pragma once

#include <cstdlib>
#include <string>

#include <rex/cvar.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace bots {

// Builds without n1 (-DBOTS_N1=OFF) offer no choice: they draw with the pre-1.0 renderer.
#if defined(BOTS_HAS_N1)
inline constexpr bool kHasN1 = true;
#else
inline constexpr bool kHasN1 = false;
#endif

// The `renderer` setting's default. The Apple TV stays on the pre-1.0 renderer until n1 is shown to fit its memory
// limit (about 2.2 GB per app, which the pre-1.0 renderer's tight-memory mode only just meets): `renderer = "new"`
// in bots.toml, or --renderer=new, opts in there.
#if defined(TARGET_OS_TV) && TARGET_OS_TV
inline constexpr const char* kDefaultRenderer = "pre1";
#else
inline constexpr const char* kDefaultRenderer = "new";
#endif

// True when the player's settings select the new renderer in this build.
inline bool UseN1() {
  if (!kHasN1) return false;
  if (rex::cvar::GetFlagByName("gpu_plugin") != "native") return false;
  return rex::cvar::GetFlagByName("renderer") != "pre1";
}

// The hybrid fill mode this run uses: BOTS_HYBRID_FILL if set (test launchers), else 4 (n1) for the new renderer.
inline int HybridFill() {
  const char* v = std::getenv("BOTS_HYBRID_FILL");
  return v && *v ? std::atoi(v) : (UseN1() ? 4 : 0);
}

}  // namespace bots
