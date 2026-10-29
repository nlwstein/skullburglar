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

## The n1 renderer

`n1/` is a Rust workspace (the optional "n1" renderer, linked into the native renderer through the C ABI in
`n1/n1ffi/include/n1ffi.h`). Every build that includes the native renderer (`-DBOTS_NATIVE_GFX=ON`, the default on
every platform) builds it with cargo as part of the CMake build; install Rust with [rustup](https://rustup.rs) and
`n1/rust-toolchain.toml` selects the version. `. tools/rust_env.sh && rust_env <target...>` (from the repo root) installs
that version and the targets a build needs; the iOS, Android and CI build scripts call it themselves.

- **Targets:** the Windows cross build needs `x86_64-pc-windows-msvc`, iOS `aarch64-apple-ios` (or `-ios-sim`),
  Android `aarch64-linux-android` (and `x86_64-linux-android` for `ABIS=x86_64`). tvOS (`aarch64-apple-tvos`) has no
  prebuilt standard library: the build compiles it from the `rust-src` component with `-Zbuild-std`, on the same
  pinned stable toolchain (`RUSTC_BOOTSTRAP=1`, set only for that cargo call).
- **GPUs:** n1 needs Vulkan 1.3 (dynamic rendering). On GPUs without the BC texture formats (most Android phones and
  headsets, the Apple TV) it decodes those textures on the CPU when it loads them.
- `-DBOTS_N1=OFF` builds without n1 (no Rust needed); the game then has only the pre-1.0 renderer.
- `-DBOTS_N1FFI_LIBRARY=<libn1ffi.a or n1ffi.lib>` links a prebuilt library instead and skips cargo.
- `cd n1 && cargo test --release` runs its unit tests; they need no game data.

## iOS and tvOS (experimental)

An iPhone, iPad or Apple TV build, with Xcode (not just its command line tools) and codegen output already in
`generated/`. There's no launcher: the game boots straight away from the app's data folder. On iOS an on-screen
controller, laid out like Xbox Cloud Gaming's touch controls, shows while no game controller is connected; tvOS needs
a paired controller.

**The iPhone build is experimental; the Apple TV build is very experimental.** The iPhone build plays (tested on an
iPhone 15 Pro Max). The Apple TV build (tested on an Apple TV 4K, 3rd generation) plays, but close to tvOS's
limit of about 2.2 GB per app (there is no entitlement to raise it). It runs in a tight-memory mode for that: the GPU
reads guest memory in place, every image and buffer gets its own allocation, and decoded BC1 textures are stored at
16 bits per texel (the A15 has no BC texture formats).

```bash
tools/ci/sdk.sh out/sdk                     # SDK with the patches, including 0023 (iOS/tvOS) and 0024
PLATFORM=ios SDK=out/sdk tools/ios/build.sh build bundle ipa     # or PLATFORM=tvos
```

- **Signing and installing:** `TEAM=<team id> BUNDLE_ID=<bundle id> DEVICE=<udid>` with the steps
  `profile sign install` signs with a development profile and installs over USB or the network (`xcrun devicectl`).
  The profile step needs the device registered to the team. A sideloading tool can sign the unsigned IPA instead.
- **Game files:** without them the app shows an import screen with a QR code, an address on port 8080 and a pairing
  code. Open the page on a phone or computer on the same network and choose your own disc image (.iso); the page reads
  the disc's file table in the browser and sends the game's files (about 6.8 GB; the device needs that much free), so
  the disc image itself is never copied. An interrupted import carries on where it stopped. On iOS the screen also has
  a Choose Disc Image button (the Files picker; the image is read in place, iCloud files are downloaded first), and an
  .iso copied into the top of the app's folder (Files app, or Finder on a Mac) is installed by itself. The files go to
  `Documents/game` on iOS (kept out of device backups) and `Library/Caches/game` on tvOS, whose apps get no persistent
  storage, so tvOS may clear them when it runs short of space. App updates keep them. For development, the `data`
  step copies `game/` over USB instead.
- **Icons:** `tools/ios/make_icons.py` (a uv script; needs the cairo library) draws the iPhone icon and the layered
  Apple TV icon and Top Shelf banners from `packaging/linux/skullburglar.svg` into `tools/ios/Assets.*.xcassets`; the
  `bundle` step compiles them with `actool`. Rerun it after changing the SVG.
- **Renderer:** on iOS n1 is the default, as on desktop. Its first start prepares its shaders for a few minutes (about
  8 on an iPhone 15 Pro Max) and keeps them, with converted textures and meshes, under `Library/Caches/bots/n1` (a few
  hundred MB). `renderer = "pre1"` in the user folder's `bots.toml` (or `--renderer=pre1`) picks the pre-1.0 renderer
  (deprecated); if n1 can't start, the error box offers that switch. The Apple TV build includes n1 but stays on the
  pre-1.0 renderer by default until n1 is shown to fit its memory limit; `renderer = "new"` (or `--renderer=new`)
  tries it.
- **Shader warm-up:** the bundle carries the macOS shader refs (`REFS`); iOS and tvOS share macOS's device class.
- **Limits:** the game quits when it goes to the background (the renderer can't pause yet). Split-screen co-op isn't
  available (it starts a second process). The guest memory needs one 4.5 GB stretch of address space: iOS asks for the
  extended virtual addressing entitlement; on the Apple TV it fits only on models with more than 3 GB of memory.
- **CI:** `build:ios` and `build:tvos` run on a Mac runner tagged `xcode`. On `main` they build with each release (or
  with `BUILD=1`), and the release links their unsigned IPAs (`skullburglar-<version>-ios.ipa`, `-tvos.ipa`); they're
  allowed to fail, and a release goes out without them then. On branches named for them (`apple`, `ios`, `tvos`) they
  build on every push, into the package registry's `skullburglar-apple/apple-<commit>`.

## Android (experimental)

An arm64 APK for Android 10 (API 29) or later with Vulkan 1.1 (1.3 for the default n1 renderer), cross-compiled with the Android NDK and packaged
without Gradle (javac, d8, aapt2, zipalign, apksigner). It needs codegen output already in `generated/` (the NDK can't
run the recompiler). Like iOS there's no launcher: the app's own first screen installs the game files, then the game
boots straight away.

**Very experimental.** It has only run in Google's arm64 Android emulator on an Apple silicon Mac (host GPU mode,
Vulkan through MoltenVK): it installs from a disc image, reaches the menus and plays a mission on the native renderer.
It hasn't run on a real device yet.

```bash
docker build -t skullburglar-android docker/android          # NDK, build tools, platform, JDK
tools/ci/sdk.sh out/sdk                                      # SDK with the patches, including 0025 (Android)
docker run --rm -v "$PWD:/w" -w /w skullburglar-android \
  sh -c 'SDK=out/sdk tools/android/build.sh build apk'       # -> out/android/Skullburglar.apk
adb install -r out/android/Skullburglar.apk
```

- **Game files:** on first start the app asks for the player's disc image (the system file picker; `ImportActivity`)
  and extracts it with the desktop launcher's `ExtractIso` into the app's data folder
  (`Android/data/<package>/files/game`, about 6.8 GB). `adb push game /sdcard/Android/data/sh.stz.skullburglar/files/`
  works too. Settings, saves and logs go to `user/` and `logs/` in the same folder. Uninstalling the app deletes them.
- **Renderer:** n1 is the default; it needs Vulkan 1.3, which the emulator has. Its first start prepares its shaders
  for a few minutes and keeps its cache (about 260 MB after one mission) in the app's internal `files/cache/n1`.
  `renderer = "pre1"` in `user/bots.toml` (or `--renderer=pre1`) picks the pre-1.0 renderer (deprecated); if n1 can't
  start, the error box offers that switch.
- **Controls:** a game controller, or the on-screen controller (`TouchPadView`, a port of the iOS one) while none is
  connected.
- **Backgrounding:** as on iOS, leaving the game quits it; the renderer can't give up its surface and resume yet.
- **Signing:** `KEYSTORE=<file>` (password `KEYSTORE_PASS`), else a debug key made in `out/android/debug.keystore` on
  first use. An APK signed with a different key needs an uninstall first, which deletes the game files.
- **Test switches:** `DEBUGGABLE=1` makes a debuggable build whose launcher passes the game's environment and command
  line on from intent extras (`adb shell am start` can't set either):
  `adb shell am start -n sh.stz.skullburglar/.ImportActivity --es env "REX_GPU_PLUGIN=native;BOTS_INPUT=22:START"`
  (pairs separated by `;`), and `--es args "..."`. The game's debug switches (`BOTS_INPUT`, `BOTS_EXEC`,
  `BOTS_CAPTURE_DIR`) work as on desktop. Release builds ignore the extras.
- **Limits:** no crash handler of our own (the system's crash report has the native backtrace).
  `ABIS="arm64-v8a x86_64"` adds x86_64 for x86 emulators, untested.
- **CI:** `build:android` runs on the Docker runner. On `main` it builds with a release (or `BUILD=1`) and the release
  includes `skullburglar-<version>-android.apk`, signed with the `ANDROID_KEYSTORE` project variable (a base64
  keystore; password `ANDROID_KEYSTORE_PASS`), so installed copies update in place. Like iOS it's allowed to fail, and
  the release then leaves it out. On branches named `android` it builds on every push, debug-signed, into the package
  registry's `skullburglar-android/android-<commit>`.

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
