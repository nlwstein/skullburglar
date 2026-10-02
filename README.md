# Skullburglar

> "Bitch took my skull."

A native PC and Mac port of the 2009 Xbox 360 game *50 Cent: Blood on the Sand*, built by statically
recompiling the game's executable with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**You need your own copy of the game.** This repository contains no game data, no game code and no
recompiled output: you extract your disc and run the recompiler yourself. See [BUILDING.md](BUILDING.md).

## Status

Early development. The game boots, menus work and the story is playable on macOS (Apple silicon,
Vulkan through MoltenVK), Windows (D3D12, cross-compiled) and Linux (Vulkan).

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
