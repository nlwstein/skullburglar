#!/bin/sh
# Live test of the portable-mode offer (B-408) with the real launcher, no game files needed:
#   tools/portable_offer_test.sh <dist dir> <work dir> [volume dir]
# <dist dir> is the flat build output (bots + libraries; macOS and Linux). <volume dir> is a folder on removable or external
# storage (the Deck's SD card under /run/media/..., a USB drive); on macOS, without it, a throwaway disk image is made and
# mounted under /Volumes (and detached again). HOME, the XDG folders and TMPDIR are a fresh sandbox under <work>, so "no
# existing install" is true. Checks, from the launcher's own log (setup: portable offer: ...):
#   removable  the offer is shown, answered Yes (BOTS_SETUP_PORTABLE=yes): portable.txt appears next to the program, the game
#              restarts itself, and the second start reads "data folder: ... (portable.txt)" and shows no offer
#   no         answered No: portable-declined.txt appears, nothing else, and the next start doesn't ask
#   later      Esc / B (later): nothing is written, and the next start asks again
#   internal   the same program on internal storage: no offer
#   existing   an existing install (saves in the default folder): no offer
# BOTS_OFFER_ONLY="removable-yes removable-no removable-later internal existing" runs only those sections.
# The run ends by itself (BOTS_SETUP_LIVE quits). One bots process at a time: refuses if one is running.
set -u
DIST=${1:?usage: portable_offer_test.sh <dist dir> <work dir> [volume dir]}
WORK=${2:?work dir}
VOL=${3:-}
OS=$(uname -s)
fail() { echo "FAIL: $*" >&2; cleanup; exit 1; }
DEV=""
cleanup() { [ -n "$DEV" ] && { hdiutil detach -quiet "$DEV" >/dev/null 2>&1 || { sleep 3; hdiutil detach -force -quiet "$DEV" >/dev/null 2>&1; }; }; DEV=""; }
[ -x "$DIST/bots" ] || fail "no $DIST/bots"
pgrep -x bots >/dev/null 2>&1 && fail "another bots process is running"
case $WORK in /|""|"$HOME") fail "refusing work dir $WORK" ;; esac
rm -rf "$WORK"; mkdir -p "$WORK/home" "$WORK/tmp" "$WORK/out" "$WORK/internal"
W=$(cd "$WORK" && pwd)
if [ -z "$VOL" ]; then
  [ "$OS" = Darwin ] || fail "give a folder on removable storage"
  hdiutil create -quiet -size 700m -fs HFS+ -volname B408OFFER -type SPARSE "$W/offer.sparseimage" || fail "hdiutil create"
  DEV=$(hdiutil attach -nobrowse "$W/offer.sparseimage" | awk 'NR==1{print $1}')
  VOL=/Volumes/B408OFFER
  [ -d "$VOL" ] || fail "disk image not mounted"
fi
passes=0
check() { if [ "$1" = ok ]; then passes=$((passes + 1)); echo "ok: $2"; else fail "$2"; fi; }
install_copy() { rm -rf "$1"; mkdir -p "$1"; if [ "$OS" = Darwin ]; then cp -cR "$DIST/." "$1/"; else cp -a "$DIST/." "$1/"; fi; }
start() {  # name appdir [env...]: runs to the end (the launcher quits itself), log in out/<name>.log
  name=$1; app=$2; shift 2
  # One bots process at a time: wait (bounded) for a foreign one to end, right before launching.
  i=0; while pgrep -x bots >/dev/null 2>&1 && [ $i -lt 180 ]; do sleep 2; i=$((i + 2)); done
  pgrep -x bots >/dev/null 2>&1 && fail "a foreign bots process kept running; not launching $name"
  env -u XDG_CACHE_HOME -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME HOME="$W/home" TMPDIR="$W/tmp" \
      BOTS_LAUNCHER=1 BOTS_SETUP_LIVE="wait:5;quit" "$@" ${BOTS_ISO_WRAP:-} "$app/bots" --log_file="$W/out/$name.log" > "$W/out/$name.out" 2>&1 &
  pid=$!
  i=0; while kill -0 "$pid" 2>/dev/null && [ $i -lt 90 ]; do sleep 1; i=$((i + 1)); done
  kill -0 "$pid" 2>/dev/null && { kill "$pid"; fail "$name still running after 90 s (stopped our pid $pid)"; }
  wait "$pid" 2>/dev/null
  sleep 4  # a restarted copy starts a second after the first quits, and runs its own 5 s
  i=0; while pgrep -x bots >/dev/null 2>&1 && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done
}
has() { grep -q -- "$2" "$W/out/$1.log"; }
sec() { [ -z "${BOTS_OFFER_ONLY:-}" ] && return 0; case " $BOTS_OFFER_ONLY " in *" $1 "*) return 0 ;; esac; return 1; }
APP=$VOL/Games/Skullburglar-b408
if sec removable-yes; then
echo "== removable: Yes =="
install_copy "$APP"
start yes "$APP" BOTS_SETUP_PORTABLE=yes
has yes "portable offer: shown" || check bad "offer shown on removable storage ($(grep 'portable offer' "$W/out/yes.log" | head -1))"
check ok "offer shown on $VOL ($(grep 'portable offer: shown' "$W/out/yes.log" | head -1 | cut -c1-160))"
[ -f "$APP/portable.txt" ] && check ok "portable.txt written next to the program" || check bad "portable.txt missing in $APP"
has yes "restarting into portable mode" && check ok "restart requested" || check bad "no restart line"
[ -d "$APP/bots-data" ] && check ok "the restarted copy made bots-data next to the program" || check bad "no bots-data after the restart"
ls "$APP"/bots-data/logs/bots-*.log >/dev/null 2>&1 && check ok "the restarted copy logs in bots-data/logs" || echo "note: no bots-data/logs log (the restarted copy had no --log_file of its own)"
fi
if sec removable-yes; then
echo "== removable: second start after Yes =="
start yes2 "$APP"
has yes2 "portable offer: not shown (portable mode is already on" && check ok "no offer once portable" || check bad "second start: $(grep 'portable offer' "$W/out/yes2.log" | head -1)"
has yes2 "data folder: .*bots-data (portable.txt)" && check ok "second start reads portable.txt" || check bad "second start is not portable"

fi
if sec removable-no; then
echo "== removable: No =="
install_copy "$APP"
start no "$APP" BOTS_SETUP_PORTABLE=no
[ -f "$APP/portable-declined.txt" ] && check ok "portable-declined.txt written" || check bad "no portable-declined.txt"
[ ! -e "$APP/portable.txt" ] && [ ! -e "$APP/bots-data" ] && check ok "No creates no portable.txt and no bots-data" || check bad "No left portable files: $(ls "$APP")"
start no2 "$APP"
has no2 "not shown (answered before" && check ok "not asked again" || check bad "asked again: $(grep 'portable offer' "$W/out/no2.log" | head -1)"

fi
if sec removable-later; then
echo "== removable: later (Esc / B) =="
install_copy "$APP"
start later "$APP" BOTS_SETUP_PORTABLE=later
[ ! -e "$APP/portable.txt" ] && [ ! -e "$APP/portable-declined.txt" ] && check ok "later writes nothing" || check bad "later wrote files: $(ls "$APP")"
start later2 "$APP"
has later2 "portable offer: shown" && check ok "asked again next start" || check bad "not asked again after later"

fi
if sec internal; then
echo "== internal storage =="
install_copy "$W/internal/app"
start internal "$W/internal/app"
has internal "not shown (not on removable or external storage" && check ok "no offer on internal storage" || check bad "internal: $(grep 'portable offer' "$W/out/internal.log" | head -1)"

fi
if sec existing; then
echo "== existing install =="
install_copy "$APP"
mkdir -p "$W/home/.local/share/bots"; echo "# an existing install" > "$W/home/.local/share/bots/bots.toml"
start existing "$APP"
has existing "not shown (an existing install" && check ok "no offer with an existing install" || check bad "existing: $(grep 'portable offer' "$W/out/existing.log" | head -1)"
rm -rf "$W/home/.local"

fi
echo "== read-only volume (no offer) is covered by the unit tests (IsRemovableOrExternal) =="
cleanup
echo "PASS: $passes checks"
