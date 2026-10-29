#!/bin/sh
# End-to-end check of the launcher's self-update on this machine, without a release or the game:
#   tools/update_probe_test.sh <macos|linux> <scratch dir>
# Builds tools/update_probe.cpp as version 0.0.1 and 0.0.2, packs both with the real tools/ci/package.sh (the macOS
# disk image, or the Linux AppImage and tarball), installs 0.0.1 in the scratch dir, serves a fake version.json signed
# with a throwaway key (made here, never kept) and the 0.0.2 packages from a local web server, and has the installed
# 0.0.1 update itself. Checks that 0.0.2 starts, that the files the player owns (a stand-in game and save folder) are
# untouched, that a listing that is unsigned, signed with another key, altered after signing, or lists an old version
# installs nothing, that a bad checksum installs nothing, that an unpacked Linux tarball and a stray $APPIMAGE never
# update, and (macOS) that the update leaves no quarantine mark where a browser download would. Never touches a real
# install or a real user folder: HOME is pointed into the scratch dir.
# macOS: INSTALL_PARENT=/Applications APP_NAME=Skullburglar-test.app puts the test install there instead, to see how
# macOS's App Management protection treats the swap.
set -eu
PLATFORM=${1:?usage: update_probe_test.sh <macos|linux> <scratch dir>}
W=$(mkdir -p "${2:?scratch dir}" && cd "$2" && pwd)
R=$(cd "$(dirname "$0")/.." && pwd)
PORT=${PORT:-18765}
case $PLATFORM in macos) ARCH=arm64 ;; linux) ARCH=x86_64 ;; *) echo "macos or linux" >&2; exit 2 ;; esac
INSTALL_PARENT=${INSTALL_PARENT:-$W/install}
APP_NAME=${APP_NAME:-Skullburglar.app}
EXTRA=""; [ "$PLATFORM" = macos ] && EXTRA="-framework CoreFoundation"
SUM="shasum -a 256"; command -v sha256sum >/dev/null && SUM=sha256sum
fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }
export TMPDIR=$W/tmp; mkdir -p "$TMPDIR"

# Two throwaway keys: the build trusts the first, the second stands in for an attacker's.
KEYS=$(python3 "$R/tools/ci/ed25519_sign.py" keygen); SEED=${KEYS% *}; PUB=${KEYS#* }
KEYS2=$(python3 "$R/tools/ci/ed25519_sign.py" keygen); SEED2=${KEYS2% *}

build() {  # version out
  c++ -std=c++20 -O1 -pthread $EXTRA -I "$R/src" -DBOTS_VERSION="\"$1\"" -DBOTS_UPDATE_URL="\"http://127.0.0.1:$PORT/version.json\"" \
    -DBOTS_UPDATE_PUBKEY="\"$PUB\"" -DBOTS_ALLOW_HTTP_UPDATE_URL=1 \
    "$R/tools/update_probe.cpp" "$R/src/setup/update_check.cpp" "$R/src/setup/update_logic.cpp" \
    "$R/src/setup/ed25519.cpp" "$R/src/setup/update_install.cpp" -o "$2"
}

pack() {  # version -> $W/pack-<version>/release
  v=$1; o=$W/pack-$v
  rm -rf "$o"; mkdir -p "$o/dist/vulkan" "$o/symbols"
  build "$v" "$o/dist/bots"
  echo "readme $v" > "$o/dist/README.txt"; echo "notices" > "$o/dist/THIRD-PARTY-NOTICES.txt"
  echo "vk $v" > "$o/dist/vulkan/loader.txt"
  if [ "$PLATFORM" = macos ]; then
    echo 'int dummy(void){return 1;}' > "$W/dummy.c"; cc -dynamiclib "$W/dummy.c" -o "$o/dist/libdummy.dylib"
  else
    echo "lib $v" > "$o/dist/libdummy.so"
  fi
  VERSION=$v "$R/tools/ci/package.sh" "$PLATFORM" "$o" >/dev/null
}

mkdir -p "$W/serve/dl/0.0.2" "$W/serve/dl/0.0.1"
for v in 0.0.1 0.0.2; do pack $v; done
for v in 0.0.1 0.0.2; do
  for f in "$W/pack-$v/release"/*; do case $f in *symbols*) ;; *) cp "$f" "$W/serve/dl/$v/" ;; esac; done
done

# mode: good | badsha (signed, but the listed checksum is wrong) | unsigned | wrongkey | tampered (size changed after
# signing) | oldver (a properly signed listing whose newest release is the one already installed)
write_json() {
python3 - "$W" "$PLATFORM" "${1:-good}" "$R" "$SEED" "$SEED2" <<'PY'
import hashlib, json, os, sys
w, platform, mode, repo, seed, seed2 = sys.argv[1:7]
sys.path.insert(0, f"{repo}/tools/ci")
import ed25519_sign as ed
version = "0.0.1" if mode == "oldver" else "0.0.2"
d = f"{w}/serve/dl/{version}"
keys = {"macos": {"macos": ".dmg"}, "linux": {"linux_appimage": ".AppImage", "linux_tar": ".tar.xz"}}[platform]
downloads = {}
for key, ext in keys.items():
    f = next(x for x in os.listdir(d) if x.endswith(ext))
    data = open(f"{d}/{f}", "rb").read()
    sha = "0" * 64 if mode == "badsha" else hashlib.sha256(data).hexdigest()
    downloads[key] = {"file": f, "size": len(data), "sha256": sha, "url": f"/dl/{version}/{f}"}
lines = sorted(f"{k} {x['file']} {x['size']} {x['sha256']}\n" for k, x in downloads.items())
msg = ("skullburglar-update-v1\nversion=" + version + "\n" + "".join(lines)).encode()
entry = {"version": version, "date": "2026-01-01", "notes": [], "available": True, "downloads": downloads}
if mode != "unsigned":
    entry["signature"] = ed.sign(bytes.fromhex(seed2 if mode == "wrongkey" else seed), msg).hex()
if mode == "tampered":
    for x in downloads.values():
        x["size"] += 1
json.dump({"latest": version, "page": "http://127.0.0.1/", "versions": [entry]}, open(f"{w}/serve/version.json", "w"), indent=1)
PY
}

(cd "$W/serve" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >"$W/server.log" 2>&1) &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true' EXIT
sleep 1

# The player's own data lives under HOME, outside the install: it must come out byte for byte the same.
export HOME=$W/home PROBE_MARKER=$W/marker
mkdir -p "$HOME/.local/share/bots-recomp/game" "$HOME/Library/Application Support/bots-recomp"
echo "my save" > "$HOME/Library/Application Support/bots-recomp/save.bin"
echo "my save" > "$HOME/.local/share/bots-recomp/game/save.bin"
tree_sum() { (cd "$HOME" && find . -type f -print0 | sort -z | xargs -0 $SUM | $SUM | cut -d' ' -f1); }
BEFORE=$(tree_sum)

wait_marker() {  # version
  i=0; while [ $i -lt 100 ]; do grep -q "started $1" "$PROBE_MARKER" 2>/dev/null && return 0; sleep 0.2; i=$((i+1)); done; return 1
}

install_old() {  # -> prints the command that opens the installed old version
  mkdir -p "$W/install"; : > "$PROBE_MARKER"
  case $PLATFORM in
    macos)
      rm -rf "${INSTALL_PARENT:?}/$APP_NAME"; mkdir -p "$INSTALL_PARENT"
      hdiutil attach -nobrowse -readonly -mountpoint "$W/mnt-old" "$W/pack-0.0.1/release/skullburglar-0.0.1-macos-arm64.dmg" >/dev/null 2>&1
      ditto "$W/mnt-old/Skullburglar.app" "$INSTALL_PARENT/$APP_NAME"; hdiutil detach -quiet "$W/mnt-old" 2>/dev/null
      echo "$INSTALL_PARENT/$APP_NAME/Contents/MacOS/bots" ;;
    linux)
      rm -rf "$W/install"; mkdir -p "$W/install"
      cp "$W/pack-0.0.1/release/skullburglar-0.0.1-linux-x86_64.AppImage" "$W/install/Skullburglar.AppImage"
      chmod +x "$W/install/Skullburglar.AppImage"; echo "$W/install/Skullburglar.AppImage" ;;
  esac
}

# The installed 0.0.1 as a version-0.0.1 marker run, and as an updater.
run_update() {  # command -> output in $W/update.out; exit status
  APPIMAGE_EXTRACT_AND_RUN=1 "$1" update >"$W/update.out" 2>&1 && return 0 || return 1
}
expect_refused() {  # label, expected text; the install must still be 0.0.1 and leave nothing behind
  CMD=$(install_old)
  if run_update "$CMD"; then cat "$W/update.out"; fail "$1: an update was installed"; fi
  grep -q "$2" "$W/update.out" || { cat "$W/update.out"; fail "$1: wrong error (wanted: $2)"; }
  case $PLATFORM in macos) /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$INSTALL_PARENT/$APP_NAME/Contents/Info.plist" | grep -q '^0.0.1$' || fail "$1: old app changed" ;; esac
  ls -A "$(dirname "$CMD")" "$INSTALL_PARENT" 2>/dev/null | grep -q '^\.skullburglar' && fail "$1: leftovers in the install folder"
  ls "$TMPDIR" | grep -q '^skullburglar-update-' && fail "$1: leftovers in the temp folder"
  ok "$1: refused, nothing installed or left behind"
}

write_json badsha;    expect_refused "bad checksum (signed)" "checksum doesn't match"
write_json unsigned;  expect_refused "unsigned listing" "isn't signed"
write_json wrongkey;  expect_refused "listing signed with another key" "isn't signed"
write_json tampered;  expect_refused "listing altered after signing" "isn't signed"
write_json oldver
PROBE_FORCE=1; export PROBE_FORCE
expect_refused "an old version, even if signed (forced)" "isn't newer"
unset PROBE_FORCE

write_json good
CMD=$(install_old)
APPIMAGE_EXTRACT_AND_RUN=1 "$CMD" >/dev/null 2>&1 || true
wait_marker 0.0.1 || fail "old version didn't start"
: > "$PROBE_MARKER"
run_update "$CMD" || { cat "$W/update.out"; fail "update failed"; }
sed 's/^/    /' "$W/update.out"
wait_marker 0.0.2 || fail "the new version didn't start after the update"
ok "updated 0.0.1 -> 0.0.2 (signed listing) and the new version started"
[ "$(tree_sum)" = "$BEFORE" ] || fail "the player's folder changed"
ok "saves and game files under HOME untouched"
case $PLATFORM in
  macos)
    /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$INSTALL_PARENT/$APP_NAME/Contents/Info.plist" | grep -q '^0.0.2$' || fail "bundle not replaced"
    codesign --verify --deep --strict "$INSTALL_PARENT/$APP_NAME" || fail "bundle signature"
    if xattr -lr "$INSTALL_PARENT/$APP_NAME" | grep -q quarantine; then fail "updated app carries a quarantine mark"; fi
    ok "app: replaced bundle verifies, and carries no com.apple.quarantine anywhere"
    ls -A "$INSTALL_PARENT" | grep -q '^\.skullburglar' && fail "leftover swap folders" ;;
  linux)
    ls -A "$W/install" | grep -q '^\.skullburglar' && fail "leftover files"
    [ "$(stat -c %a "$W/install/Skullburglar.AppImage")" = 755 ] || fail "AppImage mode"
    ls "$TMPDIR" | grep -q '^skullburglar-update-' && fail "leftover temp folder"
    # An unpacked tarball never updates itself, and neither does a program that merely inherited $APPIMAGE.
    rm -rf "$W/tar"; mkdir -p "$W/tar"; tar -xf "$W/pack-0.0.1/release/skullburglar-0.0.1-linux-x86_64.tar.xz" -C "$W/tar"
    T=$W/tar/skullburglar-0.0.1-linux-x86_64/bots
    APPIMAGE="$W/install/Skullburglar.AppImage" "$T" update >"$W/tar.out" 2>&1 && fail "a tarball install updated"
    grep -q "install kind 0, package (none)" "$W/tar.out" || { cat "$W/tar.out"; fail "tarball wasn't left on Download it"; }
    ok "tarball install (with an inherited \$APPIMAGE): no self-update"
    APPIMAGE="$W/install/Skullburglar.AppImage" APPDIR="$W/install" "$T" update >"$W/tar.out" 2>&1 && fail "a program outside \$APPDIR updated"
    grep -q "install kind 0, package (none)" "$W/tar.out" || { cat "$W/tar.out"; fail "\$APPDIR check missing"; }
    ok "\$APPIMAGE is only trusted when the program is under \$APPDIR" ;;
esac
echo "ALL PASSED ($PLATFORM)"
