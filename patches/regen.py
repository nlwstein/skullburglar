#!/usr/bin/env python3
"""Regenerate patches/rexglue-sdk/*.patch from the working tree of the SDK checkout.

Several patches touch the same files, so each diff hunk is assigned to a patch by file, or (for
shared files) by a keyword that appears in the hunk. Afterwards the series is checked with
`git apply` in order onto the base commit in a scratch worktree.

    python3 patches/regen.py [--sdk ~/repos/bots-recomp-ext/rexglue-sdk]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "rexglue-sdk")

# (patch file, whole files, {shared file: hunk keyword regex})
SERIES = [
    ("0001-codegen-real-fences-for-sync-lwsync.patch",
     ["src/codegen/builders/system.cpp"], {}),
    ("0002-debug-audio-dump-fps-log.patch",
     ["src/audio/sdl/sdl_audio_driver.cpp", "src/graphics/vulkan/deferred_command_buffer.cpp"],
     {"src/graphics/command_processor.cpp": r"REX_FPS_LOG|fps_log",
      "src/graphics/vulkan/command_processor.cpp": r"NanScan|REX_NAN_LOG|<cstdlib>|<chrono>"}),
    ("0003-vulkan-hoist-shared-memory-uploads.patch",
     ["include/rex/graphics", "src/graphics/shared_memory.cpp",
      "src/graphics/vulkan/shared_memory.cpp"],
     {"src/graphics/vulkan/command_processor.cpp": r"hoist|RenderPassEnded|CommitDrawReads"}),
    ("0004-framerate-limit-and-vk-warning-dedupe.patch",
     ["src/ui/vulkan/vulkan_instance.cpp"],
     {"src/graphics/command_processor.cpp": r"framerate_limit|<thread>"}),
    ("0005-abort-on-unhandled-host-fault.patch",
     ["src/core/exception_handler_posix.cpp"], {}),
    # deferred_command_buffer.cpp (splice-based hoist, render-area shrink) stays whole in 0002 and
    # its header in 0003; this patch is the macOS fullscreen fix, presenter debug logs and the
    # register-write micro-optimizations.
    ("0006-perf-micro-and-mac-fullscreen.patch",
     ["src/ui/vulkan/vulkan_presenter.cpp", "src/ui/windowed_app_context_sdl.cpp"],
     {"src/graphics/command_processor.cpp": r"known_registers|legacy_readback|write_one_reg",
      "src/ui/presenter.cpp": r"fullscreen|Fullscreen|paint rectangle|PRESENTER_LOG"}),
    ("0007-input-system-thread-safety.patch",
     ["include/rex/input/input_system.h", "src/input/input_system.cpp"], {}),
    # Async pipelines: no end-of-submission wait (the shader-compile hitches), background
    # precompile of the stored pipelines with progress reporting (B-053), REX_FPS_LOG shader-work /
    # skipped-draw counters, and the macOS "a+" fopen fix that wiped the storage every launch.
    # Header members for these live in the 0003 diff (include/rex/graphics is claimed whole there).
    ("0008-async-pipelines-background-precompile.patch",
     ["src/graphics/d3d12/pipeline_cache.cpp", "src/graphics/vulkan/pipeline_cache.cpp",
      "src/graphics/d3d12/command_processor.cpp", "src/graphics/graphics_system.cpp",
      "include/rex/system/interfaces/graphics.h", "src/core/filesystem_posix.cpp"],
     {"src/graphics/vulkan/command_processor.cpp":
          r"NoteShaderWork|async_skipped_draws_",
      "src/graphics/command_processor.cpp": r"shader_precompile_background"}),
]
SERIES.append(
    # Diagnostics for long play sessions (2026-10-01 crash + fps drop): host present/display
    # stats and paint-mode changes under REX_FPS_LOG, D3D12 present sync-interval cvar, guest
    # VM-op ring + fault-page dump on unhandled guest access violations (REX_VMEM_LOG=1 live), and
    # the achievement toast registering its imgui dialog only while a toast shows (a permanent
    # dialog forced the presenter into UI-thread paint mode). The fps-log additions (frames
    # >25 ms, guest memory gauge) ride in 0002's REX_FPS_LOG hunk.
    ("0009-diagnostics-present-stats-vmem-ring.patch",
     ["include/rex/ui", "src/ui/d3d12", "src/ui/overlay", "src/system/xmemory.cpp"],
     {"src/ui/presenter.cpp": r"PresentStat|PaintModeName|GetDisplayedFrameCounts|<chrono>|"
                              r"<cstdlib>|fmt/format|Presenter paint mode|UIOnlyPaint|ui_tick_forced_|<thread>"}))
SERIES.append(
    # No audio output device (headless Linux, no sound card, unplugged headset): register a silent,
    # device-paced driver instead of failing XAudioRegisterRenderDriverClient, which a real console
    # never does; BotS crashed on the null driver handle (B-066).
    ("0010-silent-audio-driver-without-device.patch",
     ["src/audio/sdl/sdl_audio_system.cpp"], {}))
SERIES.append(
    # XUserReadStats answers with an empty view per spec instead of success-without-writing, which
    # left the game parsing its uninitialized results buffer (B-059: 578 MB allocation / runaway
    # write on the main thread). Hunk-scoped: the co-op branch (B-057) edits other XGI messages.
    ("0011-xam-xuserreadstats-empty-results.patch", [],
     {"src/kernel/xam/apps/xgi_app.cpp": r"XUserReadStats|results_size|<atomic>|<cstring>"}))
SERIES.append(
    # Linux: no /proc/self/maps parse (QueryProtect) on every write-watch fault. It ran under the global
    # critical region thousands of times a second, costing the game's main thread ~25% and stalling the GPU
    # command processor on the lock; the race it guarded is covered by the access violation recovery path.
    ("0012-linux-write-watch-fault-without-maps-parse.patch",
     ["src/system/mmio_handler.cpp"], {}))
SERIES.append(
    # Linux: attach to RenderDoc only when it's already loaded. dlopen()ing it whenever it's installed (SteamOS
    # ships it in /usr/lib) injected its capture layer into every Vulkan call: ~32% of the GPU thread on the Deck.
    ("0013-linux-renderdoc-only-when-attached.patch",
     ["src/ui/renderdoc_api.cpp"], {}))
SERIES.append(
    # POSIX multi-handle waits polled with sleep_for(remaining truncated to ms): alertable waits poll in 1 ms slices,
    # so every slice slept 0 and spun - the audio worker burned ~80% of a core, as did any guest alertable
    # KeWaitForMultipleObjects.
    ("0014-posix-wait-multiple-no-spin.patch",
     ["src/core/threading_posix.cpp"], {}))
SERIES.append(
    # SpirvShaderTranslator::Features::fragment_contraction / vertex_contraction: no NoContraction decorations in pixel
    # / vertex shaders. SPIRV-Cross's Metal backend turns every decorated add/sub/mul into a [[clang::optnone]] helper
    # call - the GPU time of a frame on an M2 Pro fell 41 ms -> 11 ms without them. Default off (exact, as before). The
    # Features/SpirvBuilder members are in 0003's header diff (include/rex/graphics is claimed whole there).
    # Also Features::render_scale: a fractional render resolution (< 1) for param gen pixel positions.
    ("0015-spirv-contraction-features.patch",
     ["src/graphics/pipeline/shader/spirv_builder.cpp", "src/graphics/pipeline/shader/spirv_translator.cpp"],
     {}))


def git(sdk, *args, **kw):
    return subprocess.run(["git", "-C", sdk, *args], capture_output=True, text=True,
                          check=True, **kw).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", default=os.path.expanduser("~/repos/bots-recomp-ext/rexglue-sdk"))
    sdk = ap.parse_args().sdk

    changed = set(git(sdk, "diff", "--name-only").split())
    claimed = set()
    for name, files, shared in SERIES:
        text = git(sdk, "diff", "--", *files) if files else ""
        for f in files:
            claimed |= {c for c in changed if c == f or c.startswith(f + "/")}
        for f, pattern in shared.items():
            head, *hunks = re.split(r"(?m)^(?=@@ )", git(sdk, "diff", "--", f))
            mine = [h for h in hunks if re.search(pattern, h)]
            if mine:
                text += head + "".join(mine)
            claimed.add(f)
        with open(os.path.join(OUT, name), "w") as fh:
            fh.write(text)
        print(f"{name}: {text.count(chr(10))} lines")

    # Every hunk of a shared file must land in exactly one patch.
    for f in {f for _, _, s in SERIES for f in s}:
        _, *hunks = re.split(r"(?m)^(?=@@ )", git(sdk, "diff", "--", f))
        for h in hunks:
            owners = [n for n, _, s in SERIES if f in s and re.search(s[f], h)]
            if len(owners) != 1:
                sys.exit(f"hunk in {f} matched {owners or 'no patch'}:\n{h[:300]}")
    if changed - claimed:
        sys.exit(f"changed files not in any patch: {sorted(changed - claimed)}")

    with tempfile.TemporaryDirectory() as tmp:
        wt = os.path.join(tmp, "wt")
        git(sdk, "worktree", "add", "-q", "--detach", wt, "HEAD")
        try:
            for name, _, _ in SERIES:
                subprocess.run(["git", "-C", wt, "apply", os.path.join(OUT, name)], check=True)
            ok = subprocess.run(["git", "-C", wt, "diff", "--quiet"]).returncode != 0
            same = git(wt, "diff") == git(sdk, "diff")
        finally:
            git(sdk, "worktree", "remove", "--force", wt)
    print("series applies in order:", ok, "| reproduces working tree exactly:", same)
    sys.exit(0 if same else 1)


if __name__ == "__main__":
    main()
