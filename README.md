# Skullburglar

> "Bitch took my skull."

A native PC and Mac port of the 2009 Xbox 360 game *50 Cent: Blood on the Sand*, built by statically
recompiling the game's executable with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**You need your own copy of the game.** This repository contains no game data, no game code and no
recompiled output: you extract your disc and run the recompiler yourself. See [BUILDING.md](BUILDING.md).

## Status

Early development. The game boots, menus work and the story is playable on macOS (Apple silicon,
Vulkan through MoltenVK), Windows (D3D12, cross-compiled) and Linux (Vulkan).

## Compared with emulation

Xenia can run this game too. What a port does differently:

- **No CPU emulation.** The PowerPC code is translated to C++ ahead of time and compiled for the host CPU
  (x86-64 or ARM64), with no JIT at run time.
- **Native renderer** (the default). The game's Direct3D calls go straight to Vulkan, so there is no emulation of
  the Xbox 360 GPU's command stream and EDRAM. The emulated GPU is still available as a fallback with
  `--gpu_plugin=xenos`.
- **macOS on Apple silicon.** Xenia has no macOS build.
- **Mouse and keyboard in menus.** You can point and click on menu items, toggles and sliders.

Performance comparison (Xenia-canary vs. this port with the emulated GPU vs. the native renderer, same machine and
scene) is in progress. Measured so far, native renderer on Windows, RTX 3090 / i7-12700K at 4K output: a 36-minute
run through all nine missions held a median of 60 fps (the game's cap) in every mission, with 3 frames over 100 ms.

## How it works

ReXGlue translates the game's PowerPC executable (`default.xex`) into C++ at build time and links it
against a runtime that stands in for the Xbox 360 kernel, GPU and audio. This repository holds what
sits around that translation:

- `bots_manifest.toml`, `config/`: recompiler configuration (function boundaries, jump tables, hooks).
- `src/`: the host app, a first-run setup window, crash reporting, and hooks into game functions.
- `patches/rexglue-sdk/`: fixes and features applied on top of the pinned SDK release.
- `tools/`: analysis scripts (disassembly, cross-references, UE3 package and script tools) and build helpers.

## Not affiliated

This is an unofficial fan project. It is not affiliated with or endorsed by 50 Cent, the game's
developer or publisher, or Microsoft. All trademarks belong to their owners.

## License

[BSD 3-Clause](LICENSE). Contributions: see [CONTRIBUTING.md](CONTRIBUTING.md).
