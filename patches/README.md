# Local patches to external dependencies

Applied on top of the pinned checkouts in `~/repos/bots-recomp-ext/`. Re-apply after any
re-clone or version bump (`git -C <checkout> apply <patch>`), then rebuild + reinstall the SDK
(**both** RelWithDebInfo and Release — the bots build runs the installed Release `bin/rexglue`
for codegen) and regenerate with `FORCE=1 tools/rebuild.sh` (the codegen stamp ignores the SDK).

| Patch | Target | Why |
|-------|--------|-----|
| `rexglue-sdk/0001-codegen-real-fences-for-sync-lwsync.patch` | rexglue-sdk v0.10.0 | Codegen emitted PPC `sync`/`lwsync` as no-ops ("x86 has strong ordering"); wrong on arm64. Emits `__atomic_thread_fence` (seq_cst / acq_rel). Suspected cause of B-036. Upstream candidate (Q-003). |
| `rexglue-sdk/0002-debug-audio-dump-fps-log.patch` | rexglue-sdk v0.10.0 (after 0001) | Debug hooks, env-gated, no effect when unset: `REX_AUDIO_DUMP`, `REX_FPS_LOG`, `REX_NAN_LOG`, `REX_RP_LOG`. Not for upstream as-is. |
| `rexglue-sdk/0003-vulkan-hoist-shared-memory-uploads.patch` | rexglue-sdk v0.10.0 (after 0002) | **Perf, required for 30+ fps on Mac.** Shared-memory uploads (`vkCmdCopyBuffer` of CPU-written guest pages) used to end the open render pass; ~7k/s in gameplay = thousands of Metal render-pass restarts on a tiler. Now, if no draw already recorded in the pass reads the uploaded pages, the copy (+ barriers) is spliced into the deferred command stream *before* the pass's begin. ~98% hoisted; gameplay 22.0 → 37.5 fps (A/B runs 47/48). cvar `vulkan_hoist_shared_memory_uploads` (default on). Upstream candidate (Q-003). |
| `rexglue-sdk/0004-framerate-limit-and-vk-warning-dedupe.patch` | rexglue-sdk v0.10.0 (after 0003) | cvar `framerate_limit` (swaps/s, 0 = off; sleeps the command processor in `XE_SWAP`) for B-038 — app default 60. Vulkan debug messenger logs each distinct non-error message only at counts 1, 10, 100, … (MoltenVK primitive-restart warning spam). |
| `rexglue-sdk/0005-abort-on-unhandled-host-fault.patch` | rexglue-sdk v0.10.0 | POSIX signal handler returned when no handler claimed a SIGSEGV/SIGBUS/SIGILL → the faulting instruction re-executed forever (B-041: endless "Unhandled guest access violation" log, 100 MB). Now writes pc + fault address to stderr and `abort()`s, so the app's SIGABRT handler prints a backtrace. Upstream candidate. |
| `rexglue-sdk/0023-ios-tvos.patch` | rexglue-sdk v0.10.0 (after 0022) | Experimental iOS / tvOS builds (`tools/ios/build.sh`). CMake: `REX_APPLE_EMBEDDED` for iOS and tvOS (CMake's `IOS` is iOS only), no recompiler CLI or install rules there, MoltenVK loaded directly without the Vulkan loader, and MoltenVK's AppKit/IOKit lookups pointed at UIKit/Foundation. Code: `REX_PLATFORM_IOS`, the guest memory as a Mach memory entry mapped with `vm_map` (no `shm_open` on iOS), MoltenVK from the app bundle, SDL's `uikit` driver and `SDL_main`. |

Regenerate the whole series from the SDK working tree with `python3 patches/regen.py`: it assigns hunks to
patches by file / keyword (extend `SERIES` there for a new patch) and verifies the series reproduces the tree
exactly. 0022 and 0023 sit outside `SERIES`: they apply after it and are edited as plain diffs.
