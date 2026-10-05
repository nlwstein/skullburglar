# Skullburglar

> "Bitch took my skull."

A native PC, Mac and Steam Deck port of the 2009 Xbox 360 game *50 Cent: Blood on the Sand*, built by statically
recompiling the game's executable with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**You need your own copy of the game.** This repository contains no game data, no game code and no
recompiled output: you extract your disc and run the recompiler yourself. See [BUILDING.md](BUILDING.md).

> **This is an AI-generated project.** Nearly all of the code, reverse engineering and documentation here was written
> by an AI coding agent, directed and play-tested by a human maintainer. It is not a weekend recompile thrown over the
> wall: it is supported and improved over time, and **every issue report is investigated and resolved.** If something
> is wrong, [open an issue](#reporting-problems).

## What makes it different

A recompiler gets the game's CPU code running. Everything a player actually notices beyond that has to be built on top,
and that is where most of the work in this port has gone.

- **A native renderer.** Recompiled ports normally keep emulating the Xbox 360's GPU: its command stream, its EDRAM
  render-target memory and its shader microcode, translated on the fly every frame. That is where emulators spend
  most of their time, and recompiling the CPU code does nothing about it. Skullburglar instead takes the game's
  Direct3D calls and renders them directly with Vulkan, like a PC port would. On an RTX 3090 it runs the same scene
  at 275 fps where Xenia manages 89 and GPU emulation in this port 108, while keeping the GPU a third to a half as
  busy at 60 fps and using less memory. The emulated GPU stays available as a fallback (`--gpu_plugin=xenos`).
- **Keyboard and mouse, done properly.** Mouse look goes through the engine's own PC mouse input rather than a fake
  analog stick, so there is no stick acceleration curve or turn lag. Menus are point-and-click, including toggles and
  sliders. Every button prompt in the game (menus, HUD, tutorials, pickups) switches to keyboard and mouse icons for
  your actual key bindings, and back to controller icons the moment you pick up a pad. Keys can be rebound in the
  launcher.
- **60 fps on the Steam Deck.** The Deck holds the 60 fps cap (about 74 fps uncapped in the benchmark scene below),
  where GPU emulation runs at about 21.
- **Shader precompilation.** On first launch the port compiles the game's shaders for your GPU before the title
  screen, with a progress screen: about 8 s on an RTX 3090, 13 s on the Steam Deck, about 30 s on Apple silicon.
  After that there is no shader-compile stutter in play, and later launches reuse the cache.
- **Mac performance.** A native Apple silicon build (Vulkan through MoltenVK, tuned for Metal). An M2 Pro runs
  gameplay at well over 100 fps uncapped. Xenia has no macOS build at all.
- **Resolution and display options.** Render at the original 720p or at 1080p, 1440p or 4K, or below 720p for weak
  GPUs (Intel graphics picks a lower scale automatically). VSync, frame-rate cap, fullscreen and windowed, all from
  the launcher.

## Status

The whole story is playable from start to finish on Windows, Linux, the Steam Deck and macOS (Apple silicon). A
36-minute run through all nine missions on an RTX 3090 held a median of 60 fps in every mission.

## Upcoming major features

- **Split-screen co-op.** The original game's co-op only works over Xbox Live or system link. The goal is two players
  on one machine: two controllers, one screen.
- **Online and LAN co-op.** Stand in for Xbox Live and system link so two copies of the port can play the story
  together over a network.
- **Frame rates above 60.** The game's simulation and cutscenes assume 60 fps or less; making higher frame rates
  safe needs per-system fixes.
- **Ultrawide and field of view.** Proper 21:9 and wider support instead of letterboxing, and a field-of-view setting.
- **In-game key rebinding.** Rebinding from the settings overlay as well as the launcher.
- **Better low-end performance.** More headroom on integrated graphics and on the Deck's CPU.

## Reporting problems

Open an issue with your platform, GPU, what you were doing and the game's log (run with `--log_file=<path>` to
write one). On Windows a crash also writes `logs\crash-<pid>.dmp`; attach it if there is one. Every report is read,
reproduced where possible and fixed.

## Compared with emulation

Xenia can run this game too. What a port does differently:

- **No CPU emulation.** The PowerPC code is translated to C++ ahead of time and compiled for the host CPU
  (x86-64 or ARM64), with no JIT at run time.
- **No GPU emulation** (with the native renderer, see above).
- **macOS on Apple silicon.** Xenia has no macOS build.

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

### Load at 60 fps

RTX 3090 / i7-12700K (20 logical CPUs), Windows, 4K output, 30 s in gameplay at the 60 fps cap. CPU is process CPU
time over wall time, in cores. GPU is the whole-GPU utilisation median.

| Scene | Renderer | CPU (cores) | Busiest threads | GPU | Memory (working set) |
|---|---|---|---|---|---|
| Mission 6, helicopter | Emulated GPU | 2.45 | 99% / 98% / 21% | 61% | 1.9 GB |
| Mission 6, helicopter | Native renderer | 2.39 | 99% / 98% / 16% | **27%** | **1.4 GB** |
| Mission 2, Humvee | Emulated GPU | 2.53 | 99% / 98% / 33% | 74% | 1.5 GB |
| Mission 2, Humvee | Native renderer | 2.33 | 99% / 98% / 15% | **40%** | **1.2 GB** |

The CPU load is the same with either renderer. Two game threads spin at a full core each whatever the frame rate.
That is the game's own busy-waiting, which a recompiled port keeps. At the same frame rate the native renderer keeps
the GPU a third to a half as busy as GPU emulation does (at this light load the GPU's clocks vary between runs, so
treat the GPU column as approximate), with a smaller working set.

Xenia-canary could not be held to 60 fps on this machine: neither its `vsync` nor its `framerate_limit` setting
capped presentation (it kept presenting at its uncapped rate). Uncapped, in the same scenes, it used 3.0-3.1 cores
(two threads at a full core, a third at 56-72%) and 90-100% of the GPU.

### First launch

Longest wait for the shader precompile before the title screen on a first launch: RTX 3090 7.7 s, Steam Deck 13.4 s,
Mac mini M4 30.6 s, Apple M2 Pro 34 s. Shaders the shipped list doesn't cover compile in the background during play.

## How it works

ReXGlue translates the game's PowerPC executable (`default.xex`) into C++ at build time and links it
against a runtime that stands in for the Xbox 360 kernel, GPU and audio. This repository holds what
sits around that translation:

- `bots_manifest.toml`, `config/`: recompiler configuration (function boundaries, jump tables, hooks).
- `src/`: the host app, the native renderer (`src/gfx`), keyboard and mouse, the launcher, crash reporting, and
  hooks into game functions.
- `patches/rexglue-sdk/`: fixes and features applied on top of the pinned SDK release.
- `tools/`: analysis scripts (disassembly, cross-references, UE3 package and script tools) and build helpers.

## Not affiliated

This is an unofficial fan project. It is not affiliated with or endorsed by 50 Cent, the game's
developer or publisher, or Microsoft. All trademarks belong to their owners.

## License

[BSD 3-Clause](LICENSE). Contributions: see [CONTRIBUTING.md](CONTRIBUTING.md).
