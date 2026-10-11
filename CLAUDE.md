# Skullburglar — agent instructions

Static recompilation of an Xbox 360 Unreal Engine 3 game (50 Cent: Blood on the Sand, 2009) into
native macOS, Linux and Windows builds using the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).
Build steps are in `BUILDING.md`; contribution rules in `CONTRIBUTING.md`.

**This repo is public.** Its history is never rewritten, so anything committed stays forever.

## Hard rules

- **Never commit game data.** `game/` (extracted disc), `*.iso`, `*.xex`, `*.xxx`, `*.upk`, `*.tfc`,
  `*.bik`, shader microcode (`shaders/`), dumps and decrypted binaries are gitignored and blocked by
  the hooks. Generated C++ from codegen (`generated/`, apart from `rexglue.cmake`) is derived from the
  XEX and is never committed either.
- **No host names, user names, personal paths or private notes** in code, comments, scripts or commit
  messages. Machine-specific scripts and working notes live outside this repo.
- Enable the hooks once per clone: `git config core.hooksPath .githooks`. They run
  `tools/leak_scan.py` on every commit, commit message and push. Never bypass them with `--no-verify`.
  To scan by hand: `python3 tools/leak_scan.py tree`.
- Conventional commits (`feat:`, `fix:`, `docs:`, `chore:` …). Never rewrite pushed history.
- Test runs never use a player's real save: point them at a separate user-data root.

## Layout

- `src/` — the game app on top of the SDK: hooks, mid-asm patches, settings, platform glue.
- `patches/` — patches to the ReXGlue SDK (`regen.py` rebuilds them); applied to a v0.10.0 checkout.
- `config/`, `bots_manifest.toml` — codegen configuration.
- `tools/` — build scripts (`tools/linux/build.sh`, `tools/win-cross/build.sh`, `tools/rebuild.sh`),
  shader intake, `leak_scan.py`. Remote builds take `BUILD_HOST=user@host`.
- `docker/` — build containers.
- `tools/ci/`, `packaging/`, `.gitlab-ci.yml` — release builds (BUILDING.md "Release builds"). Pushing `main` releases
  it when there are `feat`/`fix`/`perf` commits (python-semantic-release computes the version; the CI makes the tag and
  a GitLab release, nothing is committed back). The commit type is the version bump, so pick it with care; don't create
  `v*` tags by hand. CI reads the game executable from a private assets project: anything derived from it (codegen
  output, compiled objects, packages) stays on the private GitLab instance: its package registries and its own
  runners' build caches (ccache). The one exception is the release job's copy of the player packages to the public
  download site (skullburglar.stz.sh) (`tools/ci/publish_downloads.py`). Never upload or log it anywhere else. Commits players
  will notice get a `Release-note:` trailer in plain player language; it becomes the download page's release notes.
  After changing codegen's inputs (manifest, `config/`, recompiler patches), run `tools/ci/codegen_publish.sh`.
- Not in git: `game/`, `generated/`, `out/`, `run/`, `logs/`, `dumps/`, `shaders/`.
