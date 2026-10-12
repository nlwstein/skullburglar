#!/bin/sh
# Experimental iOS / tvOS build: cross-compiles the game for an arm64 iPhone/iPad or Apple TV, bundles it as an app,
# signs it with a development profile and (with DEVICE) installs it. Needs codegen output in generated/ (build for
# macOS first, or run the recompiler) and an SDK checkout with this repo's patches (tools/ci/sdk.sh).
#
#   [PLATFORM=ios|tvos] TEAM=<team id> BUNDLE_ID=<bundle id> [DEVICE=<udid>] [SDK=<sdk path>] tools/ios/build.sh [steps]
#
# Steps (default: profile build bundle sign, plus install when DEVICE is set):
#   profile  build a stub app with Xcode's automatic signing to register BUNDLE_ID with the entitlements in
#            bots.<platform>.entitlements and get a development profile (cached in out/<platform>/). An App Store
#            Connect key in ASC_KEY_PATH / ASC_KEY_ID / ASC_ISSUER_ID is used when set, else Xcode's signed-in account.
#   build    configure and build (Ninja) into out/build/<platform>; CCACHE_DIR=<dir> compiles through ccache
#   bundle   assemble out/<platform>/Skullburglar.app, with the shader refs for the first-boot warm-up: REFS, or
#            out/assets/pipelines/class3.refs (iOS and tvOS are MoltenVK's device class 3, like macOS)
#   sign     codesign the dylibs and the app
#   ipa      zip the app into out/<platform>/Skullburglar-<platform>.ipa (signed or not: sideloading tools re-sign)
#   install  install on DEVICE (xcrun devicectl)
#   data     copy game/ into the app's data folder on DEVICE (Documents/game on iOS, Library/Caches/game on tvOS;
#            several GB; once)
# TEAM and BUNDLE_ID are needed by profile and sign only; SDK by build and bundle.
set -eu
cd "$(dirname "$0")/../.."

PLATFORM=${PLATFORM:-ios}
case $PLATFORM in
ios) SYSTEM=iOS SDKNAME=iphoneos DATA=Documents/game ;;
tvos) SYSTEM=tvOS SDKNAME=appletvos DATA=Library/Caches/game ;;
*) echo "PLATFORM must be ios or tvos" >&2; exit 1 ;;
esac
CONFIG=${CONFIG:-RelWithDebInfo}
JOBS=${JOBS:-8}
B=out/build/$PLATFORM
OUT=out/$PLATFORM
APP=$OUT/Skullburglar.app
PROFILE=$OUT/embedded.mobileprovision
ENTITLEMENTS=tools/ios/bots.$PLATFORM.entitlements

if [ $# -eq 0 ]; then
  set -- profile build bundle sign
  [ -z "${DEVICE:-}" ] || set -- "$@" install
fi

step() { echo "== $* ($(date +%H:%M:%S))"; }

do_profile() {
  : "${TEAM:?set TEAM to the Apple development team id}" "${BUNDLE_ID:?set BUNDLE_ID}"
  [ ! -f "$PROFILE" ] || { echo "profile: $PROFILE exists (delete it to refresh)"; return; }
  step profile
  P=$OUT/profile-stub
  cmake -G Xcode -S tools/ios/profile -B "$P" -DCMAKE_SYSTEM_NAME=$SYSTEM -DBUNDLE_ID="$BUNDLE_ID" -DTEAM_ID="$TEAM" \
    -DENTITLEMENTS="$(pwd)/$ENTITLEMENTS" >/dev/null
  set -- -project "$P/botsprofile.xcodeproj" -scheme bots -configuration Debug -derivedDataPath "$P/dd" \
    -allowProvisioningUpdates -allowProvisioningDeviceRegistration
  if [ -n "${DEVICE:-}" ]; then
    set -- "$@" -destination "id=$DEVICE"
  else
    set -- "$@" -destination "generic/platform=$SYSTEM"
  fi
  if [ -n "${ASC_KEY_PATH:-}" ]; then
    set -- "$@" -authenticationKeyPath "$ASC_KEY_PATH" -authenticationKeyID "$ASC_KEY_ID" \
      -authenticationKeyIssuerID "$ASC_ISSUER_ID"
  fi
  xcodebuild "$@" build >"$OUT/profile.log" 2>&1 || { tail -20 "$OUT/profile.log"; exit 1; }
  cp "$(find "$P" -name embedded.mobileprovision | head -1)" "$PROFILE"
}

do_build() {
  : "${SDK:?set SDK to an SDK checkout with the patches applied (tools/ci/sdk.sh)}"
  step configure
  [ -f generated/default/sources.cmake ] || { echo "no codegen output in generated/" >&2; exit 1; }
  # The build can't run an iOS recompiler; codegen output is already there, so its step runs `true`.
  set -- -G Ninja -S . -B "$B" -DCMAKE_SYSTEM_NAME=$SYSTEM -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE="$CONFIG" -DREXSDK_DIR="$SDK" \
    -DREXGLUE_HOST_CLI=/usr/bin/true -DBOTS_NATIVE_GFX=ON
  if [ -n "${CCACHE_DIR:-}" ]; then
    # As in tools/ci/build.sh: the sloppiness lets ccache reuse objects built with the precompiled headers.
    export CCACHE_DIR CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}" CCACHE_COMPILERCHECK="%compiler% --version"
    set -- "$@" -DBOTS_CCACHE=ON
  fi
  cmake "$@" >"$OUT/configure.log" 2>&1 || { tail -30 "$OUT/configure.log"; exit 1; }
  step build
  cmake --build "$B" --target bots rexgpu-native MoltenVK --parallel "$JOBS"
}

do_bundle() {
  step bundle
  rm -rf "$APP"
  mkdir -p "$APP"
  case $CONFIG in RelWithDebInfo) pf=rd ;; Debug) pf=d ;; *) pf= ;; esac
  # The build stages the executable and the SDK libraries in $B/bots.app; its Info.plist is replaced below.
  cp "$B/bots.app/bots" "$B/bots.app/librexruntime$pf.dylib" "$B/bots.app/librexgpu-xenos$pf.dylib" "$APP/"
  cp "$B/librexgpu-native$pf.dylib" "$APP/"
  # Loaded by path (rex/ui/vulkan/vulkan_instance.cpp); the versioned file is the real one. The SDK writes its
  # libraries into its own out/<platform>-arm64.
  : "${SDK:?set SDK to the SDK checkout the build used}"
  cp "$(ls "$SDK/out/$PLATFORM-arm64/libMoltenVK$pf".*.*.*.dylib | head -1)" "$APP/libMoltenVK.dylib"
  # Shader refs for the first-boot warm-up (src/gfx/native.cpp looks in <exe dir>/pipelines).
  refs=${REFS:-out/assets/pipelines/class3.refs}
  if [ -f "$refs" ]; then
    mkdir -p "$APP/pipelines"
    cp "$refs" "$APP/pipelines/5451083B.refs"
  else
    echo "bundle: no shader refs ($refs), so the first boot compiles shaders as they appear" >&2
  fi
  sed -e "s/@BUNDLE_ID@/${BUNDLE_ID:-io.example.skullburglar}/" -e "s/@BUILD_NUMBER@/$(date +%s)/" \
    "tools/ios/Info.$PLATFORM.plist.in" >"$APP/Info.plist"
  # Xcode adds these to every app it builds; the installer refuses a bundle without them.
  sdkver=$(xcrun --sdk $SDKNAME --show-sdk-version)
  for kv in "DTPlatformName $SDKNAME" "DTSDKName $SDKNAME$sdkver" "DTPlatformVersion $sdkver" \
            "DTSDKBuild $(xcrun --sdk $SDKNAME --show-sdk-build-version)" \
            "DTXcode $(xcodebuild -version | awk 'NR==1{split($2,v,"."); printf "%d%d%02d", v[1]/10, v[1]%10, v[2]*10}')" \
            "DTXcodeBuild $(xcodebuild -version | awk 'NR==2{print $3}')" "DTCompiler com.apple.compilers.llvm.clang.1_0"; do
    /usr/libexec/PlistBuddy -c "Add :${kv%% *} string ${kv#* }" "$APP/Info.plist"
  done
  plutil -convert binary1 "$APP/Info.plist"
  [ ! -f "$PROFILE" ] || cp "$PROFILE" "$APP/embedded.mobileprovision"
}

do_sign() {
  : "${TEAM:?set TEAM to the Apple development team id}" "${BUNDLE_ID:?set BUNDLE_ID}"
  step sign
  ident=${IDENTITY:-$(security find-identity -v -p codesigning | awk -F'"' '/Apple Development/{print $2; exit}')}
  ent=$OUT/bots.xcent
  # The platform's entitlements plus the identifiers codesign needs spelled out.
  {
    sed -n '1,/<dict>/p' "$ENTITLEMENTS"
    printf '\t<key>application-identifier</key><string>%s.%s</string>\n' "$TEAM" "$BUNDLE_ID"
    printf '\t<key>com.apple.developer.team-identifier</key><string>%s</string>\n' "$TEAM"
    printf '\t<key>get-task-allow</key><true/>\n'
    sed -n '/<dict>/,$p' "$ENTITLEMENTS" | sed 1d
  } >"$ent"
  for f in "$APP"/*.dylib; do codesign -f -s "$ident" --timestamp=none "$f" >/dev/null; done
  codesign -f -s "$ident" --timestamp=none --entitlements "$ent" "$APP"
  codesign -v "$APP" && echo "signed: $APP"
}

do_ipa() {
  step ipa
  rm -rf "$OUT/ipa" "$OUT/Skullburglar-$PLATFORM.ipa"
  mkdir -p "$OUT/ipa/Payload"
  cp -R "$APP" "$OUT/ipa/Payload/"
  (cd "$OUT/ipa" && zip -qry "../Skullburglar-$PLATFORM.ipa" Payload)
  rm -rf "$OUT/ipa"
  echo "ipa: $OUT/Skullburglar-$PLATFORM.ipa"
}

do_install() {
  : "${DEVICE:?set DEVICE to the device UDID (xcrun devicectl list devices)}"
  step install
  xcrun devicectl device install app --device "$DEVICE" "$APP"
}

do_data() {
  : "${DEVICE:?set DEVICE}" "${BUNDLE_ID:?set BUNDLE_ID}"
  step data
  # devicectl copies single files only (a directory source fails with EISDIR); it creates parent directories.
  find -L game -type f >"$OUT/data-files.txt"
  n=$(grep -c . "$OUT/data-files.txt")
  i=0
  while IFS= read -r f; do
    i=$((i + 1))
    echo "[$i/$n] $f"
    xcrun devicectl device copy to --device "$DEVICE" --domain-type appDataContainer \
      --domain-identifier "$BUNDLE_ID" --source "$f" --destination "$DATA/${f#game/}" >/dev/null
  done <"$OUT/data-files.txt"
}

mkdir -p "$OUT"
for s in "$@"; do "do_$s"; done
