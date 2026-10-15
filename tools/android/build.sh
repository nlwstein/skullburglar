#!/bin/sh
# Experimental Android build: cross-compiles the game with the NDK and packages it as an APK (no Gradle). Runs in
# docker/android (or anywhere with ANDROID_NDK, ANDROID_BUILD_TOOLS and ANDROID_PLATFORM_JAR set). Needs codegen
# output in generated/ (build for another platform first, or restore stored output) and an SDK checkout with this
# repo's patches (tools/ci/sdk.sh).
#
#   [ABIS="arm64-v8a x86_64"] [SDK=<sdk path>] [VERSION=x.y.z] tools/android/build.sh [steps]
#
# Steps (default: build apk):
#   build  configure and build (Ninja) each ABI into out/build/android-<abi>; CCACHE_DIR=<dir> compiles through ccache
#   apk    package out/android/Skullburglar.apk: SDL's Java glue and tools/android/java (javac, d8), the manifest
#          (aapt2), each ABI's libraries, zipalign, then a signature (apksigner): KEYSTORE=<file> (its first key;
#          password KEYSTORE_PASS, default android), else a debug key made in out/android/debug.keystore on first use.
#          An install over an APK signed with another key needs an uninstall, which deletes the game files.
# ABIS defaults to arm64-v8a, the devices'; x86_64 runs in x86 emulators. PACKAGE overrides the application id.
# DEBUGGABLE=1 makes a test build: debuggable, and the game takes its command line and environment from intent
# extras (SkullburglarActivity). On first start the app asks for the player's disc image (ImportActivity); the game
# files can also go in its data folder by hand: adb push game /sdcard/Android/data/<package>/files/
set -eu
cd "$(dirname "$0")/../.."

ABIS=${ABIS:-arm64-v8a}
CONFIG=Release  # no postfix on the library names, which SkullburglarActivity.getLibraries() lists
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}
MIN_SDK=29
TARGET_SDK=36
OUT=out/android
PACKAGE=${PACKAGE:-sh.stz.skullburglar}

if [ $# -eq 0 ]; then set -- build apk; fi

step() { echo "== $* ($(date +%H:%M:%S))"; }
need() { for v; do eval "[ -n \"\${$v:-}\" ]" || { echo "set $v (docker/android sets it)" >&2; exit 1; }; done; }

do_build() {
  need ANDROID_NDK
  : "${SDK:?set SDK to an SDK checkout with the patches applied (tools/ci/sdk.sh)}"
  [ -f generated/default/sources.cmake ] || { echo "no codegen output in generated/" >&2; exit 1; }
  for abi in $ABIS; do
    B=out/build/android-$abi
    step "configure $abi"
    # The build can't run the recompiler for Android; codegen output is already there, so its step runs `true`.
    set -- -G Ninja -S . -B "$B" -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-$MIN_SDK -DANDROID_STL=c++_shared \
      -DCMAKE_BUILD_TYPE=$CONFIG -DREXSDK_DIR="$SDK" -DREXGLUE_HOST_CLI=/bin/true -DBOTS_NATIVE_GFX=ON \
      -DBOTS_VERSION="${VERSION:-dev}"
    if [ -n "${CCACHE_DIR:-}" ]; then
      # As in tools/ci/build.sh: the sloppiness lets ccache reuse objects built with the precompiled headers.
      export CCACHE_DIR CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
      export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}" CCACHE_COMPILERCHECK="%compiler% --version"
      set -- "$@" -DBOTS_CCACHE=ON
    fi
    mkdir -p "$OUT"
    cmake "$@" >"$OUT/configure-$abi.log" 2>&1 || { tail -40 "$OUT/configure-$abi.log"; exit 1; }
    step "build $abi"
    cmake --build "$B" --target bots rexgpu-native rexgpu-xenos --parallel "$JOBS"
  done
}

# The NDK's name for an ABI's sysroot folder (libc++_shared.so lives there).
triple() {
  case $1 in
  arm64-v8a) echo aarch64-linux-android ;;
  x86_64) echo x86_64-linux-android ;;
  *) echo "unsupported ABI $1" >&2; exit 1 ;;
  esac
}

# The SDK's folder suffix for an ABI (out/android-<arch>).
sdkarch() {
  case $1 in
  arm64-v8a) echo arm64 ;;
  x86_64) echo amd64 ;;
  esac
}

do_apk() {
  need ANDROID_NDK ANDROID_BUILD_TOOLS ANDROID_PLATFORM_JAR
  : "${SDK:?set SDK to the SDK checkout the build used}"
  step apk
  W=$OUT/apk
  rm -rf "$W" && mkdir -p "$W/classes" "$W/dex" "$W/root"
  # SDL's Java glue (from the SDK's SDL, so it matches the native side) and the activity.
  find "$SDK/thirdparty/sdl3/android-project/app/src/main/java" tools/android/java -name '*.java' >"$W/sources.txt"
  javac -nowarn -encoding UTF-8 --release 11 -classpath "$ANDROID_PLATFORM_JAR" -d "$W/classes" @"$W/sources.txt" \
    2>&1 | grep -v '^Note:' || true
  [ -f "$W/classes/sh/stz/skullburglar/TouchPadView.class" ] || { echo "javac failed" >&2; exit 1; }
  "$ANDROID_BUILD_TOOLS/d8" --release --min-api $MIN_SDK --lib "$ANDROID_PLATFORM_JAR" --output "$W/dex" \
    $(find "$W/classes" -name '*.class')
  appver=$(echo "${VERSION:-}" | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
  # versionCode: x*10000 + y*100 + z for a release, else 1 (dev builds).
  code=$(echo "${appver:-0.0.0}" | awk -F. '{print $1*10000 + $2*100 + $3}')
  [ "$code" -gt 0 ] || code=1
  "$ANDROID_BUILD_TOOLS/aapt2" link -o "$W/base.apk" -I "$ANDROID_PLATFORM_JAR" --manifest tools/android/AndroidManifest.xml \
    --min-sdk-version $MIN_SDK --target-sdk-version $TARGET_SDK --version-code "$code" \
    --version-name "${VERSION:-dev}" --rename-manifest-package "$PACKAGE" \
    $([ "${DEBUGGABLE:-0}" = 1 ] && echo --debug-mode)
  cp "$W/dex/classes.dex" "$W/root/"
  for abi in $ABIS; do
    B=out/build/android-$abi
    L=$W/root/lib/$abi
    mkdir -p "$L"
    cp "$B/libmain.so" "$B/librexgpu-native.so" "$L/"
    # The SDK writes the runtime into its own out/<platform> folder; the xenos plugin lands in the build folder.
    for lib in librexruntime.so librexgpu-xenos.so; do
      f=$(find "$B" "$SDK/out" -name "$lib" -type f -path "*$(sdkarch "$abi")*" 2>/dev/null | head -1)
      [ -n "$f" ] || f=$(find "$B" -name "$lib" -type f | head -1)
      [ -n "$f" ] || { echo "no $lib for $abi" >&2; exit 1; }
      cp "$f" "$L/"
    done
    cp "$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$(triple "$abi")/libc++_shared.so" "$L/"
    # Keep the symbols (crash backtraces print names), drop the line tables.
    "$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug "$L"/*.so
  done
  (cd "$W/root" && zip -qr ../base.apk .)
  # 16 KB alignment for the uncompressed parts: devices with 16 KB pages need it.
  "$ANDROID_BUILD_TOOLS/zipalign" -P 16 -f 4 "$W/base.apk" "$W/aligned.apk"
  ks=${KEYSTORE:-$OUT/debug.keystore}
  if [ ! -f "$ks" ]; then
    keytool -genkeypair -keystore "$ks" -storepass android -keypass android -alias androiddebugkey -keyalg RSA \
      -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
  fi
  # The password goes through the environment, never the command line.
  KS_PASS=${KEYSTORE_PASS:-android} "$ANDROID_BUILD_TOOLS/apksigner" sign --ks "$ks" --ks-pass env:KS_PASS \
    --key-pass env:KS_PASS \
    --out "$OUT/Skullburglar.apk" "$W/aligned.apk"
  echo "apk: $OUT/Skullburglar.apk ($(du -h "$OUT/Skullburglar.apk" | cut -f1); $ABIS)"
}

mkdir -p "$OUT"
for s in "$@"; do "do_$s"; done
