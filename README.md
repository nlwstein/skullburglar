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

### Performance

Median fps with the frame cap lifted (the game normally runs at 60), same scene, warm shader caches, game resolution
1280x720 for every renderer.

- **Xenia-canary**: build 02d2cb5, vsync and frame limit off, D3D12. Measured with PresentMon; Xenia presents once
  per game frame.
- **Emulated GPU**: this port with `--gpu_plugin=xenos` (D3D12 on Windows, Vulkan on Linux). It runs the same recompiled
  CPU code as the native renderer, so the gap between those two columns is the renderer alone.
- **Native renderer**: this port's default. Measured with the game's own frame counter.

| Machine | Scene | Xenia-canary | Emulated GPU | Native renderer |
|---|---|---|---|---|
| RTX 3090 / i7-12700K, Windows, 4K output | Mission 6, helicopter | 89 | 108 | **275** |
| RTX 3090 / i7-12700K, Windows, 4K output | Mission 2, Humvee | 113 | 86 | **349** |
| Steam Deck, 1280x720 window | Mission 6, helicopter | – | 20.8 | **74.4** |
| ThinkPad T480 (Intel UHD 620), Windows | Mission 6, helicopter | – | 5.3 | **18.5** |

On the 3090 the native renderer is 3.1x Xenia in both scenes. The emulated-GPU column is slower than Xenia in the
Humvee level; the two use different render-target paths (ours ROV, Xenia's RTV), which we haven't compared yet. Xenia
on the Deck and the T480 hasn't been measured yet. The native renderer's "auto" render resolution picks 2/3 scale on
the T480, which gives 34.5 fps; the table uses full scale so the columns match.

Also on the RTX 3090, at the normal 60 fps cap: a 36-minute run through all nine missions held a median of 60 fps in
every mission, with 3 frames over 100 ms.

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
