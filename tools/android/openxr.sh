#!/bin/sh
# Fetches what headset mode (src/gfx/xr) needs into out/android/openxr: the OpenXR headers, and the Khronos loader
# the APK ships as lib/<abi>/libopenxr_loader.so.
#
# The loader has to be bundled - Meta's own copy lives in /system_ext/lib64, which an app's linker namespace does
# not expose, so an app cannot dlopen it. Khronos publishes the Android build (headers and the .so for each ABI)
# as a Maven Central aar; the GitHub releases carry no assets. Both the headers and the .so are needed, and both
# stay out of git (out/ is not tracked), so this runs on a fresh checkout before the first build.
#
#   tools/android/openxr.sh [dest]        (default out/android/openxr)
#
# build.sh runs it by itself; OPENXR_DIR=<dir> points the build at another copy. VERSION pins the release.
set -eu
cd "$(dirname "$0")/../.."

VERSION=${VERSION:-1.1.65}
DEST=${1:-out/android/openxr}

# Nothing to do once both halves are in place; build.sh calls this on every build.
if [ -f "$DEST/include/openxr/openxr.h" ] && [ -f "$DEST/lib/arm64-v8a/libopenxr_loader.so" ]; then
  exit 0
fi

echo "== OpenXR loader $VERSION -> $DEST"
mkdir -p "$DEST"
aar=$DEST/openxr_loader_for_android-$VERSION.aar
[ -f "$aar" ] || curl -fsSL -o "$aar" \
  "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$VERSION/openxr_loader_for_android-$VERSION.aar"
# The aar is a plain zip: headers under prefab/modules/headers/include/openxr, one .so per ABI under jni/<abi>.
rm -rf "$DEST/include" "$DEST/jni" "$DEST/prefab"
unzip -o -q "$aar" 'prefab/modules/headers/include/openxr/*' 'jni/*/libopenxr_loader.so' -d "$DEST"
mkdir -p "$DEST/include"
mv "$DEST/prefab/modules/headers/include/openxr" "$DEST/include/openxr"
rm -rf "$DEST/prefab"
# jni/<abi>/ -> lib/<abi>/, the layout the build and the APK step both expect.
for abi in "$DEST"/jni/*/; do
  [ -d "$abi" ] || continue
  name=$(basename "$abi")
  mkdir -p "$DEST/lib/$name"
  mv "$abi/libopenxr_loader.so" "$DEST/lib/$name/"
done
rm -rf "$DEST/jni"
echo "openxr: $DEST (headers; loader for $(cd "$DEST/lib" && ls | tr '\n' ' '))"
