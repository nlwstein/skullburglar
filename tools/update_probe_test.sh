#!/bin/sh
# End-to-end check of the launcher's self-update on this machine, without a release or the game:
#   tools/update_probe_test.sh <macos|linux> <scratch dir>
# Builds tools/update_probe.cpp as version 0.0.1 and 0.0.2, packs both with the real tools/ci/package.sh (the macOS
# disk image, or the Linux AppImage and tarball), installs 0.0.1 in the scratch dir, serves a fake version.json signed
# with a throwaway key (made here, never kept) and the 0.0.2 packages from a local web server, and has the installed
# 0.0.1 update itself. Checks that 0.0.2 starts, that the files the player owns (a stand-in game and save folder) are
# untouched, that a listing that is unsigned, signed with another key, altered after signing, or lists an old version
# installs nothing, that a bad checksum installs nothing, that a stray $APPIMAGE never makes a program an AppImage,
# that an unpacked Linux tarball updates in place (kept files, killed-mid-update states, hostile packages, no folder
# exchange, no marker), and (macOS) that the update leaves no quarantine mark where a browser download would. Never touches a real
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
# Linux: the probe is built with the test-only hooks (a forced "no folder exchange", a hold point to kill at) that a
# release build never has.
TESTDEFS=""; [ "$PLATFORM" = linux ] && TESTDEFS="-DBOTS_UPDATE_TESTING=1"
SUM="shasum -a 256"; command -v sha256sum >/dev/null && SUM=sha256sum
fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }
export TMPDIR=$W/tmp; mkdir -p "$TMPDIR"

# Two throwaway keys: the build trusts the first, the second stands in for an attacker's.
KEYS=$(python3 "$R/tools/ci/ed25519_sign.py" keygen); SEED=${KEYS% *}; PUB=${KEYS#* }
KEYS2=$(python3 "$R/tools/ci/ed25519_sign.py" keygen); SEED2=${KEYS2% *}

build() {  # version out
  c++ -std=c++20 -O1 -pthread $EXTRA -I "$R/src" -DBOTS_VERSION="\"$1\"" -DBOTS_UPDATE_URL="\"http://127.0.0.1:$PORT/version.json\"" \
    -DBOTS_UPDATE_PUBKEY="\"$PUB\"" -DBOTS_ALLOW_HTTP_UPDATE_URL=1 $TESTDEFS \
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
write_json() {  # mode [replacement .tar.xz to list as the new tarball]
python3 - "$W" "$PLATFORM" "${1:-good}" "$R" "$SEED" "$SEED2" "${2:-}" <<'PY'
import hashlib, json, os, shutil, sys
w, platform, mode, repo, seed, seed2, tar_override = sys.argv[1:8]
sys.path.insert(0, f"{repo}/tools/ci")
import ed25519_sign as ed
version = "0.0.1" if mode == "oldver" else "0.0.2"
d = f"{w}/serve/dl/{version}"
keys = {"macos": {"macos": ".dmg"}, "linux": {"linux_appimage": ".AppImage", "linux_tar": ".tar.xz"}}[platform]
downloads = {}
for key, ext in keys.items():
    f = next(x for x in os.listdir(d) if x.endswith(ext) and not x.startswith("override-"))
    if tar_override and key == "linux_tar":
        f = "override-" + os.path.basename(tar_override)
        shutil.copy(tar_override, f"{d}/{f}")
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
    # A program that merely inherited $APPIMAGE is not that AppImage: it never updates through it.
    rm -rf "$W/tar"; mkdir -p "$W/tar"; tar -xf "$W/pack-0.0.1/release/skullburglar-0.0.1-linux-x86_64.tar.xz" -C "$W/tar"
    T=$W/tar/skullburglar-0.0.1-linux-x86_64/bots
    [ -f "$W/tar/skullburglar-0.0.1-linux-x86_64/install-kind" ] || fail "the tarball has no install-kind marker"
    PROBE_MARKER="$W/m3" APPIMAGE="$W/install/Skullburglar.AppImage" APPDIR="$W/install" "$T" >/dev/null 2>&1 || true
    grep -q "install kind 4" "$W/m3" || { cat "$W/m3"; fail "an inherited \$APPIMAGE/\$APPDIR must leave the tarball install a tarball install (kind 4)"; }
    ok "an inherited \$APPIMAGE is only trusted when the program is under \$APPDIR (the tarball keeps its own kind)"

    # ---- The tarball install ----
    TI=$W/tarinst                       # the install's parent; the install itself is $TI/sb
    SB=$TI/sb
    tree_hash() { (cd "$1" && find . \( -type f -o -type l \) -print0 | sort -z | xargs -0 -I{} sh -c 'if [ -L "$1" ]; then echo "L $1 $(readlink "$1")"; else echo "F $1 $(stat -c %a "$1") $(sha256sum < "$1")"; fi' _ {} | sha256sum | cut -d" " -f1); }
    mine_hash() { (cd "$TI" && { tree_hash gamedata; tree_hash sb/logs; $SUM < sb/mynotes.txt; readlink sb/game; $SUM < sb/game/save.dat; } | $SUM | cut -d" " -f1); }
    install_tar() {  # a fresh 0.0.1 tarball install with the player's own files in it
      rm -rf "$TI"; mkdir -p "$TI/x"; : > "$PROBE_MARKER"
      tar -xf "$W/pack-0.0.1/release/skullburglar-0.0.1-linux-x86_64.tar.xz" -C "$TI/x"
      mv "$TI/x/skullburglar-0.0.1-linux-x86_64" "$SB"; rmdir "$TI/x"
      mkdir -p "$TI/gamedata/C1Game" "$SB/logs"
      head -c 200000 /dev/urandom > "$TI/gamedata/C1Game/big.bin"; echo "save in the game folder" > "$TI/gamedata/save.dat"
      ln -s ../gamedata "$SB/game"                 # a relative link, as a player who keeps the game elsewhere has
      echo "log line" > "$SB/logs/bots.log"; echo "my notes" > "$SB/mynotes.txt"
    }
    ver_of() { sed -n 's/^readme //p' "$SB/README.txt"; }
    consistent() {  # label: the whole install folder is one version (never a mix) and has what the player keeps
      v=$(ver_of); [ -n "$v" ] || fail "$1: no README"
      grep -qx "lib $v" "$SB/libdummy.so" || fail "$1: libdummy.so isn't $v (mixed folder)"
      grep -qx "vk $v" "$SB/vulkan/loader.txt" || fail "$1: vulkan/loader.txt isn't $v (mixed folder)"
      [ -f "$SB/install-kind" ] || fail "$1: marker missing"
      : > "$W/m2"; PROBE_MARKER="$W/m2" "$SB/bots" || fail "$1: bots won't run"
      grep -q "started $v " "$W/m2" || fail "$1: bots isn't $v (mixed folder)"
      echo "$v"
    }
    age_leftovers() { for d in "$TI"/.skullburglar-update-* "$TMPDIR"/skullburglar-update-*; do [ -e "$d" ] && touch -d '2 hours ago' "$d"; done; return 0; }
    expect_tar_refused() {  # label, expected text
      install_tar; B1=$(tree_hash "$SB"); M1=$(mine_hash)
      if "$SB/bots" update >"$W/update.out" 2>&1; then cat "$W/update.out"; fail "$1: an update was installed"; fi
      grep -q "$2" "$W/update.out" || { cat "$W/update.out"; fail "$1: wrong error (wanted: $2)"; }
      [ "$(tree_hash "$SB")" = "$B1" ] || fail "$1: the install folder changed"
      [ "$(mine_hash)" = "$M1" ] || fail "$1: the player's files changed"
      ls -A "$TI" | grep -q '^\.skullburglar' && fail "$1: leftovers next to the install"
      ls "$TMPDIR" | grep -q '^skullburglar-update-' && fail "$1: leftovers in the temp folder"
      [ ! -e "$TI/evil" ] && [ ! -e "$W/abs-evil" ] || fail "$1: a member escaped the folder"
      ok "tarball: $1: refused, install and the player's files untouched, nothing left behind"
    }
    write_json badsha;    expect_tar_refused "bad checksum (signed)" "checksum doesn't match"
    write_json unsigned;  expect_tar_refused "unsigned listing" "isn't signed"
    write_json wrongkey;  expect_tar_refused "listing signed with another key" "isn't signed"
    write_json tampered;  expect_tar_refused "listing altered after signing" "isn't signed"
    write_json oldver; PROBE_FORCE=1; export PROBE_FORCE
    expect_tar_refused "an old version, even if signed (forced)" "isn't newer"; unset PROBE_FORCE

    # Signed, correct checksum, but the package itself is hostile: refused before anything is unpacked.
    python3 - "$W" "$W/abs-evil" <<'PY'
import io, sys, tarfile
w, absname = sys.argv[1:3]
def build(name, extra):
    with tarfile.open(f"{w}/{name}.tar.xz", "w:xz") as t:
        def add(n, data=b"x", mode=0o755, typ=tarfile.REGTYPE, link=""):
            i = tarfile.TarInfo(n); i.mode = mode; i.type = typ; i.linkname = link
            i.size = len(data) if typ == tarfile.REGTYPE else 0
            t.addfile(i, io.BytesIO(data) if typ == tarfile.REGTYPE else None)
        add("p/", typ=tarfile.DIRTYPE); add("p/bots"); add("p/install-kind", b"linux-tarball\n", 0o644)
        extra(add)
build("evil-dotdot", lambda add: add("../evil"))
build("evil-nested-dotdot", lambda add: add("p/a/../../evil"))
build("evil-absolute", lambda add: add(absname))
build("evil-symlink-abs", lambda add: add("p/l", typ=tarfile.SYMTYPE, link="/etc"))
build("evil-symlink-up", lambda add: add("p/l", typ=tarfile.SYMTYPE, link="../../etc"))
build("evil-hardlink", lambda add: add("p/h", typ=tarfile.LNKTYPE, link="p/bots"))
build("evil-fifo", lambda add: add("p/f", typ=tarfile.FIFOTYPE))
build("evil-two-tops", lambda add: add("q/x"))
build("no-program", lambda add: None)
PY
    rm "$W/evil-no-program.tar.xz" 2>/dev/null || true
    python3 - "$W" <<'PY'
import io, tarfile, sys
w = sys.argv[1]
with tarfile.open(f"{w}/no-program.tar.xz", "w:xz") as t:    # a clean package with no bots program in it
    for n, typ, data in (("p/", tarfile.DIRTYPE, b""), ("p/install-kind", tarfile.REGTYPE, b"linux-tarball\n")):
        i = tarfile.TarInfo(n); i.type = typ; i.mode = 0o755; i.size = len(data)
        t.addfile(i, io.BytesIO(data) if data else None)
with tarfile.open(f"{w}/no-marker.tar.xz", "w:xz") as t:     # a clean package without the marker
    for n, typ, data in (("p/", tarfile.DIRTYPE, b""), ("p/bots", tarfile.REGTYPE, b"x")):
        i = tarfile.TarInfo(n); i.type = typ; i.mode = 0o755; i.size = len(data)
        t.addfile(i, io.BytesIO(data) if data else None)
PY
    write_json good "$W/evil-dotdot.tar.xz";        expect_tar_refused "member ../evil" "climbs out"
    write_json good "$W/evil-nested-dotdot.tar.xz"; expect_tar_refused "member p/a/../../evil" "climbs out"
    write_json good "$W/evil-absolute.tar.xz";      expect_tar_refused "absolute-path member" "absolute path"
    write_json good "$W/evil-symlink-abs.tar.xz";   expect_tar_refused "symlink to an absolute path" "link that leaves"
    write_json good "$W/evil-symlink-up.tar.xz";    expect_tar_refused "symlink climbing out" "link that leaves"
    write_json good "$W/evil-hardlink.tar.xz";      expect_tar_refused "hard link member" "special file"
    write_json good "$W/evil-fifo.tar.xz";          expect_tar_refused "fifo member" "special file"
    write_json good "$W/evil-two-tops.tar.xz";      expect_tar_refused "two top-level folders" "more than one top-level"
    write_json good "$W/no-program.tar.xz";         expect_tar_refused "package without the program" "no bots program"
    write_json good "$W/no-marker.tar.xz";          expect_tar_refused "package without the marker" "isn't a tarball package"

    # An install folder without the marker (a build tree, an odd copy) is never touched.
    write_json good
    install_tar; rm "$SB/install-kind"; B1=$(tree_hash "$SB")
    "$SB/bots" update >"$W/update.out" 2>&1 && fail "an install without the marker updated"
    grep -q "install kind 0, package (none)" "$W/update.out" || { cat "$W/update.out"; fail "no-marker install wasn't left on Download it"; }
    [ "$(tree_hash "$SB")" = "$B1" ] || fail "no-marker install changed"
    ok "tarball: a folder without the install-kind marker is left alone (kind 0)"

    # A marker file that is a link, or the wrong text, doesn't count either.
    install_tar; rm "$SB/install-kind"; echo "something else" > "$SB/install-kind"
    "$SB/bots" update >"$W/update.out" 2>&1 && fail "a wrong marker updated"
    grep -q "install kind 0" "$W/update.out" || fail "wrong marker text accepted"
    ok "tarball: a marker with the wrong content doesn't count"

    # A file system without folder exchange: refused up front, nothing attempted, the install untouched.
    install_tar; B1=$(tree_hash "$SB"); M1=$(mine_hash)
    BOTS_UPDATE_TEST_NO_EXCHANGE=1 "$SB/bots" update >"$W/update.out" 2>&1 && fail "updated without a folder exchange"
    grep -q "can't swap folders in one step" "$W/update.out" || { cat "$W/update.out"; fail "no-exchange wasn't reported"; }
    [ "$(tree_hash "$SB")" = "$B1" ] && [ "$(mine_hash)" = "$M1" ] || fail "no-exchange run changed things"
    ls -A "$TI" | grep -q '^\.skullburglar' && fail "no-exchange left folders behind"
    ok "tarball: no RENAME_EXCHANGE -> refused (\"$(grep -o 'blocked: .*' "$W/update.out")\"), Download link only, install untouched"

    # Killed mid-update: at each stage of the swap (held by the test build), the folder is whole, old or new.
    for point in extracted carried exchanged; do
      install_tar; M1=$(mine_hash); rm -f "$W/hold"
      BOTS_UPDATE_TEST_HOLD="$point:$W/hold" "$SB/bots" update >"$W/update.out" 2>&1 &
      PID=$!; i=0; while [ ! -e "$W/hold" ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
      [ -e "$W/hold" ] || { cat "$W/update.out"; fail "never reached $point"; }
      kill -9 $PID; wait $PID 2>/dev/null || true
      V=$(consistent "killed at $point")
      case $point in extracted|carried) [ "$V" = 0.0.1 ] || fail "killed at $point but the install is $V" ;; exchanged) [ "$V" = 0.0.2 ] || fail "killed after the exchange but the install is $V" ;; esac
      # The next start's clean-up puts back anything caught in the carry window and removes the stage.
      age_leftovers; "$SB/bots" clean >/dev/null 2>&1 || true
      [ "$(mine_hash)" = "$M1" ] || fail "killed at $point: the player's files are not intact after clean-up"
      ls -A "$TI" | grep -q '^\.skullburglar' && fail "killed at $point: stage left after clean-up"
      ls "$TMPDIR" | grep -q '^skullburglar-update-' && fail "killed at $point: temp left after clean-up"
      consistent "after clean-up ($point)" >/dev/null
      ok "tarball: killed at '$point': folder is entirely $V, the player's files intact, clean-up leaves nothing"
    done
    # And at random moments of a real update.
    for delay in 0.02 0.05 0.1 0.15 0.2 0.3 0.4 0.55 0.7 0.9; do
      install_tar; M1=$(mine_hash)
      "$SB/bots" update >"$W/update.out" 2>&1 &
      PID=$!; sleep "$delay"; kill -9 $PID 2>/dev/null || true; wait $PID 2>/dev/null || true
      sleep 2.5; pkill -f "$SB/bots" 2>/dev/null || true
      V=$(consistent "random kill at ${delay}s")
      age_leftovers; "$SB/bots" clean >/dev/null 2>&1 || true
      [ "$(mine_hash)" = "$M1" ] || fail "random kill at ${delay}s: the player's files changed"
      ls -A "$TI" | grep -q '^\.skullburglar' && fail "random kill at ${delay}s: stage left after clean-up"
      printf "ok: tarball: killed at %ss: folder is whole (%s), files intact\n" "$delay" "$V"
    done

    # The good update. The updater runs from inside the folder it swaps (a running program in the old folder).
    install_tar; M1=$(mine_hash); INO=$(stat -c %i "$SB/bots")
    "$SB/bots" >/dev/null 2>&1 || true
    wait_marker 0.0.1 || fail "old version didn't start"
    : > "$PROBE_MARKER"
    "$SB/bots" update >"$W/update.out" 2>&1 || { cat "$W/update.out"; fail "tarball update failed"; }
    sed 's/^/    /' "$W/update.out"
    wait_marker 0.0.2 || fail "the new version didn't start after the tarball update"
    grep "started 0.0.2" "$PROBE_MARKER" | grep -q "install at $SB " || { cat "$PROBE_MARKER"; fail "the new version isn't running from the same path"; }
    V=$(consistent "after the update"); [ "$V" = 0.0.2 ] || fail "not updated"
    [ "$(mine_hash)" = "$M1" ] || fail "the player's files changed in the update"
    [ -L "$SB/game" ] && [ "$(readlink "$SB/game")" = ../gamedata ] || fail "the game link changed"
    [ "$(stat -c %i "$SB/bots")" != "$INO" ] || fail "bots wasn't replaced"
    [ "$(stat -c %a "$SB")" = 755 ] || fail "folder mode"
    [ "$(stat -c %a "$SB/bots")" = 755 ] || fail "bots mode"
    i=0; while ls -A "$TI" | grep -q '^\.skullburglar' && [ $i -lt 40 ]; do sleep 0.25; i=$((i+1)); done
    ls -A "$TI" | grep -q '^\.skullburglar' && fail "the old folder wasn't deleted after the new version started"
    ls "$TMPDIR" | grep -q '^skullburglar-update-' && fail "leftover temp folder"
    ok "tarball: updated 0.0.1 -> 0.0.2 in place: same path, game link/logs/notes byte for byte, new bots, old folder deleted"
    # The updated install updates no further (0.0.2 is the newest listed).
    "$SB/bots" update >"$W/update.out" 2>&1 || true; grep -q "up to date" "$W/update.out" || fail "updated install isn't up to date"
    ok "tarball: the new install reports up to date"
esac
echo "ALL PASSED ($PLATFORM)"
