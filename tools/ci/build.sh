#!/bin/sh
# Release build for one platform: codegen, compile, then stage the files to ship with debug info split off.
#   tools/ci/build.sh <linux|windows|macos> <sdk dir> <out dir>
# Runs inside the platform's build environment (the release workflow sets these up):
#   linux    docker/linux-steamrt (Steam Runtime 3: glibc 2.31, static libstdc++; runs on the Deck and most distros)
#   windows  docker/win-cross (clang + lld cross-compiling to x86_64 MSVC; Windows SDK from xwin in $XWIN)
#   macos    an Apple silicon Mac with Xcode or its command line tools, cmake and ninja
# Needs game/default.xex (checked against default.xex.sha1) and an SDK from tools/ci/sdk.sh. Codegen output already in
# generated/ (tools/ci/codegen_publish.sh) is reused when its stamp matches; otherwise codegen runs here.
# Optional: REFS=<file> ships it as pipelines/5451083B.refs (shader refs for the first-boot warm-up, B-098);
# JOBS=<n> caps compile parallelism (default: all cores); CODEGEN_ONLY=1 stops after codegen;
# CCACHE_DIR=<dir> compiles through ccache with its cache there (CI keeps one per runner and platform, CCACHE_MAXSIZE
# default 10G): an unchanged codegen key then reuses the compiled generated code; VERSION=<x.y.z> and UPDATE_URL=<url>
# give the launcher its version and the releases JSON it checks for a newer one (src/setup/update_check.h).
# Output: <out>/dist (what ships), <out>/symbols (debug info for crash reports).
set -eu
PLATFORM=${1:?usage: tools/ci/build.sh <linux|windows|macos> <sdk dir> <out dir>}
SDK=$(cd "${2:?sdk dir}" && pwd)
mkdir -p "${3:?out dir}"
OUT=$(cd "$3" && pwd)
cd "$(dirname "$0")/../.."
B=$OUT/build
DIST=$OUT/dist
SYM=$OUT/symbols
case $PLATFORM in macos) JOBS=${JOBS:-$(sysctl -n hw.ncpu)} ;; *) JOBS=${JOBS:-$(nproc)} ;; esac

if command -v sha1sum >/dev/null; then sha1sum -c default.xex.sha1; else shasum -c default.xex.sha1; fi

CCACHE=
if [ -n "${CCACHE_DIR:-}" ]; then
  # The precompiled headers are rebuilt every time; this sloppiness lets ccache reuse them and the objects built
  # with them (CMakeLists.txt adds -fno-pch-timestamp under BOTS_CCACHE).
  export CCACHE_DIR CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
  export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}"
  # By version, not the mtime of the compiler (a wrapper script in the Linux image, new with every image rebuild).
  export CCACHE_COMPILERCHECK="%compiler% --version"
  mkdir -p "$CCACHE_DIR"
  ccache -z >/dev/null
  CCACHE=1
fi

step() { echo "== $* ($(date -u +%H:%M:%S))"; }

# n1 (n1/, Rust) is built by the CMake build with cargo. The Linux and Windows images carry the toolchain; on the Mac it
# is the user's rustup, found by its usual places or, when there is none, installed here without touching shell
# profiles (rustup-init into ~/.cargo, user-local). n1/rust-toolchain.toml selects the version, which rustup fetches
# on first use.
if [ "$PLATFORM" = macos ] && ! command -v cargo >/dev/null; then
  for d in "$HOME/.cargo/bin" /opt/homebrew/opt/rustup/bin /usr/local/opt/rustup/bin; do
    [ ! -x "$d/cargo" ] || { PATH="$d:$PATH"; break; }
  done
  if ! command -v cargo >/dev/null; then
    step "rustup (user-local install)"
    curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain none
    PATH="$HOME/.cargo/bin:$PATH"
  fi
  export PATH
fi

# Codegen output already in generated/ (restored by the caller for an unchanged codegen key) is used as it is: the
# build's codegen step runs `true` instead of the recompiler. The recompiler's own up-to-date check can't be trusted
# for this: it misses on some builds and regenerates identical output for half an hour.
NOGEN=
if [ -f generated/default/sources.cmake ]; then
  # A path: `command -v true` names the shell builtin.
  for NOGEN in /usr/bin/true /bin/true; do [ ! -x "$NOGEN" ] || break; done
fi

case $PLATFORM in
linux)
  # x86-64-v2 (SSSE3/SSE4) is the SDK's own x86 minimum.
  set -- -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DREXSDK_DIR="$SDK" \
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DBOTS_NATIVE_GFX=ON
  ;;
windows)
  # The build can't run the rexglue.exe it would build, so codegen runs a recompiler built for Linux first.
  if [ -z "$NOGEN" ]; then
    step "recompiler (host)"
    cmake -G Ninja -S "$SDK" -B "$OUT/host" -DCMAKE_BUILD_TYPE=Release \
      ${CCACHE:+-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache} \
      -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 >/dev/null
    cmake --build "$OUT/host" --target rexglue --parallel "$JOBS"
  fi
  # MSVC header names are matched case-insensitively on Windows; add the spellings the sources use.
  python3 tools/win-cross/casefix.py "$SDK" src
  set -- -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/win-cross/win-clang.cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DREXSDK_DIR="$SDK" -DREXGLUE_HOST_CLI="${NOGEN:-$SDK/out/linux-amd64/rexglue}" -DREXGLUE_USE_VULKAN=ON -DBOTS_NATIVE_GFX=ON
  ;;
macos)
  set -- --preset mac-arm64-relwithdebinfo -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 -DREXSDK_DIR="$SDK" -DBOTS_NATIVE_GFX=ON
  ;;
*) echo "unknown platform $PLATFORM" >&2; exit 1 ;;
esac

step configure
[ -z "$NOGEN" ] || [ "$PLATFORM" = windows ] || set -- "$@" -DREXGLUE_HOST_CLI="$NOGEN"
set -- "$@" -DBOTS_VERSION="${VERSION:-dev}" -DBOTS_UPDATE_URL="${UPDATE_URL:-}"
cmake -S . -B "$B" "$@" -DBOTS_CCACHE=$([ -n "$CCACHE" ] && echo ON || echo OFF) >/dev/null
if [ ! -f generated/default/sources.cmake ]; then
  # The generated sources are listed at configure time, so configure again once codegen has written them.
  step codegen
  cmake --build "$B" --target bots_codegen --parallel "$JOBS"
  cmake -S . -B "$B" >/dev/null
fi
[ -z "${CODEGEN_ONLY:-}" ] || exit 0
step build
cmake --build "$B" --target bots --parallel "$JOBS"
[ -z "$CCACHE" ] || ccache -s

step stage
rm -rf "$DIST" "$SYM"
mkdir -p "$DIST" "$SYM"
# The game, the SDK runtime and both GPU plugins (native, and the emulated Xenos one the launcher can switch to).
# Library names carry the build type's postfix ("rd" for RelWithDebInfo).
case $PLATFORM in
linux)
  cp "$B/bots" "$B"/librexgpu-native*.so "$DIST/"
  cp "$SDK"/out/linux-amd64/librexruntime*.so "$SDK"/out/linux-amd64/librexgpu-xenos*.so "$DIST/"
  for f in "$DIST"/*; do
    n=$(basename "$f")
    llvm-objcopy --only-keep-debug "$f" "$SYM/$n.debug"
    llvm-objcopy --strip-debug --add-gnu-debuglink="$SYM/$n.debug" "$f"
  done
  # bots has RUNPATH $ORIGIN: every library it needs from the build has to be next to it.
  if LD_LIBRARY_PATH= ldd "$DIST/bots" | grep "not found"; then exit 1; fi
  ;;
windows)
  cp "$B/bots.exe" "$B"/rexgpu-native*.dll "$DIST/"
  cp "$SDK"/out/win-amd64/rexruntime*.dll "$SDK"/out/win-amd64/rexgpu-xenos*.dll "$DIST/"
  for f in "$DIST"/*.exe "$DIST"/*.dll; do
    n=$(basename "${f%.*}")
    for pdb in "$B/$n.pdb" "$SDK/out/win-amd64/$n.pdb"; do [ ! -f "$pdb" ] || cp "$pdb" "$SYM/"; done
  done
  ;;
macos)
  # The build copies the SDK libraries and the Vulkan loader + MoltenVK (vulkan/) next to the game.
  cp "$B/bots" "$B"/librexruntime*.dylib "$B"/librexgpu-*.dylib "$DIST/"
  cp -R "$B/vulkan" "$DIST/"
  for f in "$DIST"/bots "$DIST"/*.dylib; do
    dsymutil "$f" -o "$SYM/$(basename "$f").dSYM"
    strip -S "$f"
    codesign --force --sign - "$f" 2>/dev/null  # stripping voids the linker's ad-hoc signature
  done
  if otool -L "$DIST/bots" | grep -E "^\s+/(opt|usr/local)/"; then echo "links a Homebrew library" >&2; exit 1; fi
  ;;
esac
if [ -n "${REFS:-}" ]; then
  mkdir -p "$DIST/pipelines"
  cp "$REFS" "$DIST/pipelines/5451083B.refs"
fi
cp packaging/README.txt "$DIST/"
# Our license, the SDK's and every SDK dependency's, and n1's Rust crates, from the SDK checkout this was built from.
python3 tools/ci/third_party_notices.py "$SDK" "$DIST/THIRD-PARTY-NOTICES.txt" n1/Cargo.toml
ls -la "$DIST"
du -sh "$DIST" "$SYM"
