# Building

You need your own copy of the game (the Xbox 360 disc image). Everything game-derived stays on your
machine: the extracted disc in `game/`, the recompiled C++ in `generated/`, and the build in `out/`, all
gitignored.

macOS on Apple silicon is the primary platform. Linux and Windows builds run in Docker (below).

## 1. ReXGlue SDK

The build uses ReXGlue **v0.10.0** with this repo's patches applied. Check it out next to this repo or
anywhere else, and set `SDK` to its path (the scripts default to `~/repos/bots-recomp-ext/rexglue-sdk`).

```bash
brew install cmake ninja vulkan-loader molten-vk
SDK=~/src/rexglue-sdk
git clone --recursive --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git $SDK
for p in patches/rexglue-sdk/*.patch; do git -C $SDK apply "$PWD/$p"; done
cd $SDK
cmake --preset mac-arm64 -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3
cmake --build out/build/mac-arm64 --config RelWithDebInfo --target install --parallel 10
cmake --build out/build/mac-arm64 --config Release --target install --parallel 10   # the rexglue CLI installs only in Release
```

Install registers the SDK for CMake's `find_package(rexglue)`. Rebuild and reinstall both configurations
whenever the patches change. What each patch does: [patches/README.md](patches/README.md).

## 2. Game files

Extract the disc into `game/` so that `game/default.xex` and `game/C1Game/` exist. Check it with
`shasum -c default.xex.sha1` (Windows: `certutil -hashfile game\default.xex SHA1`); other builds of the game
won't match the recompiler configuration.

```bash
docker run --rm --user $(id -u):$(id -g) -e CARGO_HOME=/tmp/cargo -v $PWD:/w -w /w rust:1-slim \
  bash -c "cargo install -q xdvdfs-cli && /tmp/cargo/bin/xdvdfs unpack bots.iso game"
rm -rf 'game/$SystemUpdate'
```

The app's first-run setup window can also extract an ISO for you.

## 3. Recompile and build (macOS)

```bash
cmake --preset mac-arm64-relwithdebinfo -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3
cmake --build out/build/mac-arm64-relwithdebinfo --parallel 6 -- -k 0
```

The build runs codegen (the recompiler) first, which takes several minutes and writes `generated/`. It reruns
when the manifest, `config/` or the SDK's CLI changes. `tools/rebuild.sh` does codegen, a pre-flight label
check and the build with numbered logs; after rebuilding the SDK, run it with `FORCE=1`.

Each generated file is about 1 MB of C++; `--parallel 6` keeps memory use reasonable on 32 GB.

## 4. Run

```bash
./out/build/mac-arm64-relwithdebinfo/bots --game_data_root=game --user_data_root=run/user \
  --cache_root=run/cache --log_file=logs/run.log
```

`tools/run.sh [seconds] [extra args]` runs with a timeout and prints the exit status, first errors and the
log tail. Giving `--game_data_root` skips the launcher (players get it on every start); `--launcher` opens it
anyway. F11 toggles fullscreen; F3 shows the debug overlay. Debug environment variables (`BOTS_*` for scripted
input, frame capture and console commands; `REX_*` for SDK settings) are read in `src/bots_app.h` and
`src/game_hooks.cpp`.

## Linux and Windows

Both build in throwaway Docker containers after codegen has produced `generated/` (on macOS as above).
They use this machine's Docker, or a remote Docker host with `BUILD_HOST=user@host`.

```bash
tools/linux/build.sh       # Vulkan; image from docker/linux/Dockerfile -> ~/bots/linux/build/bots
tools/win-cross/build.sh   # D3D12, clang + xwin -> ~/bots/win/dist/bots-win64.zip
```

Running the Linux build needs a Vulkan GPU passed into the container (`--gpus all` and
`NVIDIA_DRIVER_CAPABILITIES=all` for NVIDIA) and a display.

## iOS and tvOS (experimental)

An iPhone, iPad or Apple TV build, with Xcode (not just its command line tools) and codegen output already in
`generated/`. There's no launcher: the game boots straight away from the app's data folder. On iOS an on-screen
controller, laid out like Xbox Cloud Gaming's touch controls, shows while no game controller is connected; tvOS needs
a paired controller.

**The iPhone build is experimental; the Apple TV build is very experimental.** The iPhone build plays (tested on an
iPhone 15 Pro Max). The Apple TV build (tested on an Apple TV 4K, 3rd generation) reaches the main menu but is
stopped by tvOS on loading a level: tvOS caps an app at about 2.2 GB there, with no entitlement to raise it, and the
game needs more.

```bash
tools/ci/sdk.sh out/sdk                     # SDK with the patches, including 0023 (iOS/tvOS)
PLATFORM=ios SDK=out/sdk tools/ios/build.sh build bundle ipa     # or PLATFORM=tvos
```

- **Signing and installing:** `TEAM=<team id> BUNDLE_ID=<bundle id> DEVICE=<udid>` with the steps
  `profile sign install` signs with a development profile and installs over USB or the network (`xcrun devicectl`).
  The profile step needs the device registered to the team. A sideloading tool can sign the unsigned IPA instead.
- **Game files:** without them the app shows an import screen with a QR code, an address on port 8080 and a pairing
  code. Open the page on a phone or computer on the same network and choose your own disc image (.iso); the page reads
  the disc's file table in the browser and sends the game's files (about 6.8 GB; the device needs that much free), so
  the disc image itself is never copied. An interrupted import carries on where it stopped. The files go to
  `Documents/game` on iOS (kept out of device backups) and `Library/Caches/game` on tvOS, whose apps get no persistent
  storage, so tvOS may clear them when it runs short of space. App updates keep them. For development, the `data`
  step copies `game/` over USB instead.
- **Icons:** `tools/ios/make_icons.py` (a uv script; needs the cairo library) draws the iPhone icon and the layered
  Apple TV icon and Top Shelf banners from `packaging/linux/skullburglar.svg` into `tools/ios/Assets.*.xcassets`; the
  `bundle` step compiles them with `actool`. Rerun it after changing the SVG.
- **Shader warm-up:** the bundle carries the macOS shader refs (`REFS`); iOS and tvOS share macOS's device class.
- **Limits:** the game quits when it goes to the background (the renderer can't pause yet). Split-screen co-op isn't
  available (it starts a second process). The guest memory needs one 4.5 GB stretch of address space: iOS asks for the
  extended virtual addressing entitlement; on the Apple TV it fits only on models with more than 3 GB of memory.
- **CI:** `build:ios` and `build:tvos` run on branches named for them (`apple`, `ios`, `tvos`) and on `main` with
  `BUILD=1`, on a Mac runner tagged `xcode`. They upload unsigned IPAs to the package registry and aren't part of
  releases.

## Release builds

`tools/ci/` builds release packages from a clean checkout (CI does all three): `sdk.sh` clones the pinned SDK and
applies `patches/`, `build.sh` runs codegen, compiles with the native renderer and stages the files to ship (debug
info split off), and `package.sh` packs them:

| Platform | Build environment | Packages |
|---|---|---|
| Linux x86_64 | `docker/linux-steamrt` (glibc 2.31: runs on the Steam Deck and most distros) | `.tar.xz`, `.AppImage` |
| Windows x86_64 | `docker/win-cross` (clang + lld, Windows SDK from xwin, NSIS) | `-setup.exe` |
| macOS arm64 | Xcode or its command line tools, cmake, ninja | `.dmg` (`Skullburglar.app`) |

Each also has a `-symbols` archive. To make one yourself, with `game/default.xex` in place:

```bash
tools/ci/sdk.sh out/sdk
docker build -t skullburglar-linux docker/linux-steamrt
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:$PWD" -w "$PWD" skullburglar-linux \
  tools/ci/build.sh linux out/sdk out/ci
VERSION=dev tools/ci/package.sh linux out/ci     # -> out/ci/release/
```

In this repository's GitLab CI (`.gitlab-ci.yml`), `main` builds Linux, Windows and macOS when it has releasable commits
(`feat`, `fix`, `perf`; versions from [python-semantic-release](https://python-semantic-release.readthedocs.io/)) and
makes a GitLab release whose packages stay in the project's private package registry. The game executable comes from
a private assets project that only this project's CI jobs can read. The other jobs run on every push and need no game
data.

When the project sets them as CI variables, the release also reaches players: `UPDATE_URL` (the address of the releases
JSON) is built into the launcher, which then checks for newer versions (`src/setup/update_check.h`; the `check_updates`
setting turns it off), and `DOWNLOADS_BUCKET` plus `DOWNLOADS_PAGE` make the release job copy the player packages to
that Cloudflare R2 bucket behind the public download page and add the release to its `version.json`
(`tools/ci/publish_downloads.py`; it keeps the newest three versions downloadable). Notes there come from the
`feat`/`fix`/`perf` commits: give a player-visible commit a `Release-note:` trailer written for players
(`Release-note: none` leaves it out).

Codegen takes over half an hour on an x86 CI runner, and its output is the same on every platform and only changes
with its inputs (`tools/ci/codegen_key.sh`: the manifest, `config/`, the recompiler's SDK sources). After changing one
of those, store the new output for the release builds with `GITLAB_TOKEN=... tools/ci/codegen_publish.sh` (on a Mac
it takes a few minutes); until then they run codegen themselves. Compiling the generated code is the other long step
(over half an hour for Windows); the builds go through ccache (`CCACHE_DIR`, kept on each runner per platform), so
while the codegen key stays the same, only changed sources are compiled again.
