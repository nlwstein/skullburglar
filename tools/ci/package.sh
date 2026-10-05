#!/bin/sh
# Pack a tools/ci/build.sh output into release files, named skullburglar-<version>-<platform>-<arch>.*
#   VERSION=0.1.0 tools/ci/package.sh <linux|windows|macos> <out dir>
# Writes <out>/release/:
#   linux    .tar.xz (the game folder) and .AppImage (the same files, one executable), symbols .tar.xz
#   windows  .zip, symbols .zip (PDBs)
#   macos    .dmg (Skullburglar.app, an Applications link and the README), symbols .zip (dSYMs)
# The AppImage is built with appimagetool and the type 2 runtime, both pinned by checksum.
set -eu
PLATFORM=${1:?usage: tools/ci/package.sh <linux|windows|macos> <out dir>}
OUT=$(cd "${2:?out dir}" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:?set VERSION}
case $PLATFORM in linux|windows) ARCH=x86_64 ;; macos) ARCH=arm64 ;; *) echo "unknown platform $PLATFORM" >&2; exit 1 ;; esac
NAME=skullburglar-$VERSION-$PLATFORM-$ARCH
REL=$OUT/release
PKG=$OUT/pkg
rm -rf "$REL" "$PKG"
mkdir -p "$REL" "$PKG"
cp -R "$OUT/dist" "$PKG/$NAME"
cp -R "$OUT/symbols" "$PKG/$NAME-symbols"
cd "$PKG"

fetch() {  # url sha256 dest
  curl -fsSL --retry 3 -o "$3" "$1"
  echo "$2  $3" | sha256sum -c --quiet
}

case $PLATFORM in
linux)
  tar -cJf "$REL/$NAME.tar.xz" "$NAME"
  tar -cJf "$REL/$NAME-symbols.tar.xz" "$NAME-symbols"
  A=$PKG/AppDir
  cp -R "$NAME" "$A"
  cp "$REPO/packaging/linux/AppRun" "$REPO/packaging/linux/skullburglar.desktop" "$REPO/packaging/linux/skullburglar.svg" "$A/"
  chmod +x "$A/AppRun"
  fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
    ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 appimagetool
  fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
    2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d runtime
  chmod +x appimagetool
  # Extract-and-run: no FUSE needed on the build machine.
  ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 ./appimagetool --no-appstream --runtime-file runtime "$A" "$REL/$NAME.AppImage"
  ;;
windows)
  zip -qr "$REL/$NAME.zip" "$NAME"
  zip -qr "$REL/$NAME-symbols.zip" "$NAME-symbols"
  ;;
macos)
  # Skullburglar.app: the game and its libraries in Contents/MacOS, the Vulkan loader + MoltenVK (the SDK looks in
  # Contents/Resources/vulkan), shader refs and the icon in Contents/Resources. Skullburglar.icns is
  # packaging/linux/skullburglar.svg at 824 px on a 1024 px transparent canvas, through iconutil.
  APP=$PKG/dmg/Skullburglar.app
  mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
  ditto "$NAME/bots" "$APP/Contents/MacOS/bots"
  for f in "$NAME"/*.dylib; do ditto "$f" "$APP/Contents/MacOS/$(basename "$f")"; done
  ditto "$NAME/vulkan" "$APP/Contents/Resources/vulkan"
  [ ! -d "$NAME/pipelines" ] || ditto "$NAME/pipelines" "$APP/Contents/Resources/pipelines"
  cp "$REPO/packaging/macos/Skullburglar.icns" "$APP/Contents/Resources/"
  sed "s/@VERSION@/$VERSION/g" "$REPO/packaging/macos/Info.plist" > "$APP/Contents/Info.plist"
  plutil -lint "$APP/Contents/Info.plist"
  # Ad-hoc signature over the whole bundle (it seals Info.plist and Resources); not notarized.
  codesign --force --deep --sign - "$APP"
  codesign --verify --deep --strict "$APP"
  cp "$NAME/README.txt" "$PKG/dmg/"
  ln -s /Applications "$PKG/dmg/Applications"
  hdiutil create -quiet -volname "Skullburglar $VERSION" -srcfolder "$PKG/dmg" -fs HFS+ -format UDZO -ov \
    "$REL/$NAME.dmg"
  # ditto keeps the symlinks inside the dSYMs intact.
  ditto -c -k --keepParent "$NAME-symbols" "$REL/$NAME-symbols.zip"
  ;;
esac
ls -la "$REL"
