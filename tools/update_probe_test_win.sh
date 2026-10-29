#!/bin/sh
# The Windows half of tools/update_probe_test.sh, run on a Linux Docker host with Wine (not on Windows):
#   tools/update_probe_test_win.sh <scratch dir>        # IMG_BUILD / IMG_WINE override the images
# Cross-builds tools/update_probe.cpp as 0.0.1 and 0.0.2 with the release build image (docker/win-cross), packs both
# with the real tools/ci/package.sh (the NSIS setup program), then in a Wine image: installs 0.0.1 silently, serves a
# fake releases file, and has the installed 0.0.1 update itself (download, check, run the setup silently, which waits
# for it to quit, replaces the program files and starts 0.0.2). Checks the new version started, the player's save,
# portable game folder and logs are untouched, a bad checksum or an unsigned listing installs nothing, an unzipped copy
# never self-updates, and a setup that times out (the game kept running) starts no second copy and leaves a note. Wine is not Windows: it can't show
# SmartScreen or a Mark of the Web, so that part still needs a real machine (docs/plans/update-button.md).
set -eu
W=$(mkdir -p "${1:?scratch dir}" && cd "$1" && pwd)
R=$(cd "$(dirname "$0")/.." && pwd)
IMG_BUILD=${IMG_BUILD:-skullburglar-ci-windows}
IMG_WINE=${IMG_WINE:-bots-wine-img}
U="$(id -u):$(id -g)"
rm -rf "$W/src" "$W/win" "$W/winpack-"*
mkdir -p "$W/src" "$W/win"
(cd "$R" && tar -c src tools packaging) | tar -x -C "$W/src"
# A throwaway signing key (the build trusts its public half; the private half stays in this scratch dir only).
KEYS=$(python3 "$R/tools/ci/ed25519_sign.py" keygen); SEED=${KEYS% *}; PUB=${KEYS#* }
echo "$SEED" > "$W/seed"

cat > "$W/build.sh" <<'IN'
set -eu
cd /work/src; X=/w/xwin
for v in 0.0.1 0.0.2; do
  clang++ --target=x86_64-pc-windows-msvc -std=c++20 -O1 -fuse-ld=lld -D_CRT_SECURE_NO_WARNINGS -I src \
    -idirafter $X/crt/include -idirafter $X/sdk/include/ucrt -idirafter $X/sdk/include/um -idirafter $X/sdk/include/shared \
    -L$X/crt/lib/x86_64 -L$X/sdk/lib/um/x86_64 -L$X/sdk/lib/ucrt/x86_64 \
    -DBOTS_VERSION=\"$v\" -DBOTS_UPDATE_URL=\"http://127.0.0.1:18765/version.json\" \
    -DBOTS_UPDATE_PUBKEY=\"$PUB\" -DBOTS_ALLOW_HTTP_UPDATE_URL=1 \
    tools/update_probe.cpp src/setup/update_check.cpp src/setup/update_logic.cpp src/setup/ed25519.cpp \
    src/setup/update_install.cpp -lwinhttp -lshell32 -ladvapi32 -o /work/win/bots-$v.exe
  o=/work/winpack-$v; mkdir -p $o/dist/pipelines $o/symbols
  cp /work/win/bots-$v.exe $o/dist/bots.exe; echo "readme $v" > $o/dist/README.txt; echo n > $o/dist/THIRD-PARTY-NOTICES.txt
  echo "dll $v" > $o/dist/libdummy.dll; echo p > $o/dist/pipelines/p.bin
  VERSION=$v tools/ci/package.sh windows $o >/dev/null
done
chown -R $OWNER /work
IN
docker run --rm -e OWNER="$U" -e PUB="$PUB" -v "$W:/work" "$IMG_BUILD" sh /work/build.sh

cat > "$W/wine.sh" <<'IN'
set -u
export WINEPREFIX=/work/wineprefix WINEDEBUG=-all WINEARCH=win64
fail() { echo "FAIL: $*"; exit 1; }
rm -rf "$WINEPREFIX" /work/winserve /work/marker /work/note.out; mkdir -p /work/winserve/dl/0.0.2
cp /work/winpack-0.0.2/release/skullburglar-0.0.2-windows-x86_64-setup.exe /work/winserve/dl/0.0.2/
mkjson() { python3 - "$1" <<'PY'
import hashlib, json, os, sys
sys.path.insert(0, "/work/src/tools/ci")
import ed25519_sign as ed
mode = sys.argv[1]
d = "/work/winserve/dl/0.0.2"; f = os.listdir(d)[0]; data = open(f"{d}/{f}", "rb").read()
sha = "0" * 64 if mode == "bad" else hashlib.sha256(data).hexdigest()
dl = {"windows": {"file": f, "size": len(data), "sha256": sha, "url": f"/dl/0.0.2/{f}"}}
msg = ("skullburglar-update-v1\nversion=0.0.2\n" + f"windows {f} {len(data)} {sha}\n").encode()
entry = {"version": "0.0.2", "available": True, "downloads": dl}
if mode != "unsigned":
    entry["signature"] = ed.sign(bytes.fromhex(open("/work/seed").read().strip()), msg).hex()
json.dump({"latest": "0.0.2", "page": "http://127.0.0.1/", "versions": [entry]}, open("/work/winserve/version.json", "w"))
PY
}
mkjson bad
(cd /work/winserve && exec python3 -m http.server 18765 --bind 127.0.0.1 >/work/winserve.log 2>&1) &
sleep 1
wineboot -i >/dev/null 2>&1
wine /work/winpack-0.0.1/release/skullburglar-0.0.1-windows-x86_64-setup.exe /S '/D=C:\Games\Skullburglar' || fail "setup 0.0.1"
I=$WINEPREFIX/drive_c/Games/Skullburglar
LA=$(echo $WINEPREFIX/drive_c/users/*/AppData/Local | cut -d' ' -f1)
mkdir -p "$LA/bots-recomp/game" $I/game; echo "my save" > "$LA/bots-recomp/game/save.bin"
echo portable > $I/game/default.xex; echo log > $I/logs.txt
SUM=$(cd "$LA/bots-recomp" && find . -type f | sort | xargs sha256sum | sha256sum)
export PROBE_MARKER='Z:\work\marker'
wine 'C:\Games\Skullburglar\bots.exe'; grep -q "started 0.0.1" /work/marker || fail "0.0.1 didn't start"; : > /work/marker
wine 'C:\Games\Skullburglar\bots.exe' update > /work/update.out 2>&1 && fail "a bad checksum was installed"
grep -q "checksum doesn't match" /work/update.out || { cat /work/update.out; fail "wrong error for a bad checksum"; }
grep -q "readme 0.0.1" $I/README.txt || fail "install changed after a refused update"
echo "ok: bad checksum refused, install unchanged"
mkjson unsigned
wine 'C:\Games\Skullburglar\bots.exe' update > /work/update.out 2>&1 && fail "an unsigned listing was installed"
grep -q "isn't signed" /work/update.out || { cat /work/update.out; fail "wrong error for an unsigned listing"; }
grep -q "readme 0.0.1" $I/README.txt || fail "install changed after an unsigned listing"
echo "ok: unsigned listing refused, install unchanged"
mkjson good
# A copy of the install that the setup program didn't make (even with uninstall.exe in it) never updates itself.
cp -r $I $WINEPREFIX/drive_c/unzipped
wine 'C:\unzipped\bots.exe' update > /work/update.out 2>&1 && fail "an unzipped copy updated"
grep -q "install kind 0, package (none)" /work/update.out || { cat /work/update.out; fail "unzipped copy not left on Download it"; }
rm -f $WINEPREFIX/drive_c/unzipped/uninstall.exe
wine 'C:\unzipped\bots.exe' update > /work/update.out 2>&1 && fail "an unzipped copy updated"
grep -q "install kind 0, package (none)" /work/update.out || { cat /work/update.out; fail "unzipped copy (no uninstall.exe) not left on Download it"; }
grep -q "readme 0.0.1" $I/README.txt && [ -f $WINEPREFIX/drive_c/unzipped/bots.exe ] || fail "an unzipped copy was touched"
echo "ok: a copy the setup program didn't make never self-updates"
# The setup run by hand with /UPDATE over a folder it didn't make: error level 3, no empty folder left by a bad /D=, the
# files untouched, and (the game isn't running) the old exe is started again.
SETUP=Z:\\work\\winpack-0.0.2\\release\\skullburglar-0.0.2-windows-x86_64-setup.exe
wine $SETUP /S /UPDATE '/D=C:\nowhere'; [ $? = 3 ] || fail "setup over a missing folder didn't exit 3"
[ ! -e $WINEPREFIX/drive_c/nowhere ] || fail "a bad /D= left a folder behind"
: > /work/marker
wine $SETUP /S /UPDATE '/D=C:\unzipped'; [ $? = 3 ] || fail "setup over an unzipped copy didn't exit 3"
[ -f $WINEPREFIX/drive_c/unzipped/bots.exe ] && ! grep -q "readme 0.0.2" $WINEPREFIX/drive_c/unzipped/README.txt || fail "setup touched an unzipped copy"
for i in $(seq 1 20); do grep -q "started 0.0.1" /work/marker 2>/dev/null && break; sleep 0.5; done
grep -q "started 0.0.1" /work/marker || fail "an unlocked failed update didn't start the old version again"
echo "ok: setup refuses a folder it didn't make (exit 3, no folder created, files untouched), old exe restarted"
# The game keeps running (bots.exe locked), so the silent setup gives up after 30 s. It must NOT start the old version
# again then (that would be a second copy of the running game), and the next start says the update didn't finish.
: > /work/marker
wine 'C:\Games\Skullburglar\bots.exe' hold >/dev/null 2>&1 &
HOLD=$!
sleep 3
wine 'C:\Games\Skullburglar\bots.exe' update | sed 's/^/    /'
sleep 50   # the setup waits 30 s for the lock, then fails
! grep -q "started" /work/marker || fail "a failed update started a second copy of the locked game"  # hold writes no marker
kill $HOLD 2>/dev/null; wineserver -k 2>/dev/null; sleep 2
grep -q "readme 0.0.1" $I/README.txt || fail "install changed by a failed update"
wine 'C:\Games\Skullburglar\bots.exe' note | tee /work/note.out
grep -q "didn't finish" /work/note.out || fail "no note about the update that didn't finish"
wine 'C:\Games\Skullburglar\bots.exe' note | tr -d '\r' | grep -q "^note: $" || fail "the note was shown twice"
echo "ok: a setup that gave up (game locked) started no second copy and left a note (shown once)"
: > /work/marker
wine 'C:\Games\Skullburglar\bots.exe' update | sed 's/^/    /'
for i in $(seq 1 80); do grep -q "started 0.0.2" /work/marker 2>/dev/null && break; sleep 0.5; done
grep -q "started 0.0.2" /work/marker || fail "0.0.2 didn't start after the update"
grep -q "readme 0.0.2" $I/README.txt || fail "program files not replaced"
echo "ok: setup ran silently, replaced the program files and started 0.0.2"
[ "$SUM" = "$(cd "$LA/bots-recomp" && find . -type f | sort | xargs sha256sum | sha256sum)" ] || fail "save changed"
[ "$(cat $I/game/default.xex)" = portable ] && [ -f $I/logs.txt ] || fail "game folder or logs touched"
echo "ok: save, portable game folder and logs untouched"
echo "ALL PASSED (windows, under Wine)"
chown -R $OWNER /work 2>/dev/null || true
IN
docker run --rm -u "$U" -e HOME=/tmp/h -e OWNER="$U" -v "$W:/work" "$IMG_WINE" sh -c 'mkdir -p /tmp/h && sh /work/wine.sh'
