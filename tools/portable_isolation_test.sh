#!/bin/sh
# Portable-mode isolation test (B-408): proves a portable session writes nothing outside its own folder.
#   tools/portable_isolation_test.sh <dist dir> <game dir> <work dir>
# <dist dir> is the flat build output (bots, libraries; macOS and Linux), <game dir> an extracted game (read only: the
# portable bots.toml names it as the game folder), <work dir> a scratch folder (deleted and recreated).
# It copies the build to <work>/run/app with a portable.txt next to it, points HOME, the XDG folders and TMPDIR at empty
# folders under <work>/run, and runs three sessions with the game's test instrumentation:
#   launcher   the launcher alone: settings changed and changed back (auto-save), then quit
#   play       launcher -> Play -> game boot -> a short run in a chapter (BOTS_ISO_CHAPTER, default 11010) -> clean quit
#   crash      launcher -> Play -> front end -> a deliberate host fault, to see where the crash archive goes
# The portable root is the folder the program was started from: the one holding the executable, portable.txt and bots-data
# (<work>/run/app here). Every write the game and its children make outside it (and the harness's own <work>/out) is recorded: on macOS
# by running the game under sandbox-exec with an allow-and-report rule for writes (the kernel's own log, read back with
# `log show`; nothing is blocked), on Linux by strace -f (BOTS_ISO_STRACE=1; the container needs SYS_PTRACE). Writes made
# for the game by system services (macOS cfprefsd's preferences, the Metal compiler service's cache) don't show in the
# trace, so the real places a program can leak to are also listed: the real /tmp, the real home's Library, ~/.cache,
# ~/.local ... and the macOS per-user temp and cache folders (only entries that carry the game's names count there,
# since other programs write to those places too). Then it removes the entries in tools/portable_isolation_allow.txt
# (each with its reason) and fails if any is left.
# Env: BOTS_ISO_ISOLATE=0 runs the build with BOTS_PORTABLE_ISOLATE=0 (what leaked before B-408), BOTS_ISO_WRAP="xvfb-run -a"
# (a prefix for the game command, Linux without a display), BOTS_ISO_PORT (control port, default 47480),
# BOTS_ISO_EXE_NAME=<name> (rename the program in the copy: the macOS preferences file is named after it, so a new name shows a first run),
# BOTS_ISO_ENV="NAME=value ..." (extra environment for the game), BOTS_ISO_CHAPTER, BOTS_ISO_PLAY_SECS (default 15), BOTS_ISO_PHASES (default "launcher play crash"; "canary" is the harness's own check: a file written inside the
# portable folder must pass and one written in the folder above it must be reported, run it alone or with the others), BOTS_ISO_KEEP=1 (keep <work>).
# Never touches a player's folders: the run's HOME is the sandbox, and the real places are only listed.
set -u
DIST=${1:?usage: portable_isolation_test.sh <dist dir> <game dir> <work dir>}
GAME=${2:?game dir}
WORK=${3:?work dir}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=${BOTS_ISO_PORT:-47480}
CHAPTER=${BOTS_ISO_CHAPTER:-11010}
PLAY_SECS=${BOTS_ISO_PLAY_SECS:-15}
PHASES=${BOTS_ISO_PHASES:-"launcher play crash"}
ISOLATE=${BOTS_ISO_ISOLATE:-1}
OS=$(uname -s)
fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$DIST/bots" ] || fail "no $DIST/bots"
[ -f "$GAME/default.xex" ] || fail "no game in $GAME"
# One bots process per machine, and ours is the only one this script ever signals (by pid).
pgrep -x bots >/dev/null 2>&1 && fail "another bots process is running; not starting"
case $WORK in /|""|"$HOME") fail "refusing work dir $WORK" ;; esac
rm -rf "$WORK"; mkdir -p "$WORK/run/home" "$WORK/run/systmp" "$WORK/run/app" "$WORK/out"
W=$(cd "$WORK" && pwd)
H=$W/run/home; T=$W/run/systmp; APP=$W/run/app; OUT=$W/out
if [ "$OS" = Darwin ]; then cp -cR "$DIST/." "$APP/"; else cp -al "$DIST/." "$APP/" 2>/dev/null || cp -a "$DIST/." "$APP/"; fi
EXE=${BOTS_ISO_EXE_NAME:-bots}
[ "$EXE" = bots ] || mv "$APP/bots" "$APP/$EXE"
: > "$APP/portable.txt"
mkdir -p "$APP/bots-data/user"
printf 'game_data_root = "%s"\n' "$GAME" > "$APP/bots-data/user/bots.toml"
# The places a program can leak to outside the sandbox: listed before and after (files newer than the marker).
REAL_HOME=$(eval echo "~$(id -un)")
WATCHFILE=$W/watch.txt
printf '/tmp\n/var/tmp\n' > "$WATCHFILE"
if [ "$OS" = Darwin ]; then
  for d in Library/Preferences Library/Logs Library/Caches "Library/Saved Application State" Library/HTTPStorages \
           "Library/Application Support" Library/Containers "Library/Group Containers" Library/Cookies Library/WebKit; do
    printf '%s\n' "$REAL_HOME/$d" >> "$WATCHFILE"
  done
  for k in DARWIN_USER_CACHE_DIR DARWIN_USER_TEMP_DIR DARWIN_USER_DIR; do
    p=$(getconf $k 2>/dev/null) && printf '%s\n' "$p" >> "$WATCHFILE"
  done
  printf '/Library/Logs/DiagnosticReports\n' >> "$WATCHFILE"
fi
for d in .local .cache .config .nv .steam; do printf '%s\n' "$REAL_HOME/$d" >> "$WATCHFILE"; done
touch "$W/marker"; sleep 1
T0=$(date '+%Y-%m-%d %H:%M:%S')
PREFIX=""
if [ "$OS" = Darwin ]; then
  # Allow everything, but report each write outside the portable folder and this script's own output folder.
  cat > "$OUT/sandbox.sb" <<SB
(version 1)
(allow default)
(allow file-write* (require-all (require-not (subpath "$APP")) (require-not (subpath "$OUT")) (require-not (subpath "/dev")) (require-not (literal "/dev/null"))) (with report))
SB
  PREFIX="sandbox-exec -f $OUT/sandbox.sb"
elif [ "${BOTS_ISO_STRACE:-0}" = 1 ]; then
  PREFIX="strace -f -qq -e trace=%file,%desc,rename,renameat,renameat2,mkdir,mkdirat,symlink,symlinkat,link,linkat,unlink,unlinkat,rmdir,truncate,chmod -s 300 -o $OUT/strace-@NAME@.txt"  # one file per session
fi
ISOENV=""; [ "$ISOLATE" = 0 ] && ISOENV="BOTS_PORTABLE_ISOLATE=0"

ctl() { python3 "$HERE/instr/botsctl.py" --port "$PORT" --connect-timeout "${CTL_WAIT:-120}" cmd "$@"; }
run_game() {  # name, extra env..., -- game args
  name=$1; shift
  : > "$OUT/$name.log"
  PFX=$(printf '%s' "$PREFIX" | sed "s|@NAME@|$name|")
  # shellcheck disable=SC2086
  env -u XDG_CACHE_HOME -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME \
      HOME="$H" TMPDIR="$T" TEMP="$T" TMP="$T" BOTS_INSTR="$PORT" $ISOENV ${BOTS_ISO_ENV:-} "$@" \
      ${BOTS_ISO_WRAP:-} $PFX "$APP/$EXE" --log_level=info ${BOTS_ISO_ARGS:-} > "$OUT/$name.out" 2>&1 &
  PID=$!
  echo "$name: started pid $PID" | tee -a "$OUT/steps.txt"
}
wait_exit() {  # seconds
  i=0
  while kill -0 "$PID" 2>/dev/null && [ $i -lt "$1" ]; do sleep 1; i=$((i + 1)); done
  if kill -0 "$PID" 2>/dev/null; then echo "  still running after $1 s: stopping our pid $PID" | tee -a "$OUT/steps.txt"; kill "$PID"; sleep 3; kill -9 "$PID" 2>/dev/null; return 1; fi
  wait "$PID" 2>/dev/null; echo "  exit status $?" | tee -a "$OUT/steps.txt"; return 0
}
for phase in $PHASES; do
  pgrep -x bots >/dev/null 2>&1 && fail "a bots process exists before the $phase session"
  case $phase in
    launcher)
      printf 'focus:intro_movies\nkey:Enter\nframes:20\nfocus:intro_movies\nkey:Enter\nframes:20\n' > "$OUT/live.txt"
      run_game launcher BOTS_LAUNCHER=1 BOTS_SETUP_LIVE="@$OUT/live.txt" BOTS_SETUP_LIVE_QUIT=1
      wait_exit 120 ;;
    play)
      run_game play BOTS_LAUNCHER=1 BOTS_SETUP_AUTO=1
      ctl wait -t 240 'phase==front_end || phase==in_play' >> "$OUT/ctl.txt" 2>&1 || echo "  front end not reached" | tee -a "$OUT/steps.txt"
      ctl exec "LoadChapter $CHAPTER" >> "$OUT/ctl.txt" 2>&1
      ctl wait -t 240 'phase==in_play' >> "$OUT/ctl.txt" 2>&1 || echo "  in_play not reached" | tee -a "$OUT/steps.txt"
      sleep "$PLAY_SECS"
      ctl shot "$OUT/play.png" >> "$OUT/ctl.txt" 2>&1
      ctl quit >> "$OUT/ctl.txt" 2>&1
      wait_exit 90 ;;
    crash)
      run_game crash BOTS_LAUNCHER=1 BOTS_SETUP_AUTO=1
      ctl wait -t 240 'phase==front_end || phase==in_play' >> "$OUT/ctl.txt" 2>&1 || echo "  front end not reached" | tee -a "$OUT/steps.txt"
      ctl crash >> "$OUT/ctl.txt" 2>&1
      wait_exit 60 ;;
    canary)
      # The harness's own check, run under the same trace as the game: one write inside the portable folder (must not be
      # reported) and one in the folder above it (must be).
      PFX=$(printf '%s' "$PREFIX" | sed "s|@NAME@|canary|")
      # shellcheck disable=SC2086
      env HOME="$H" TMPDIR="$T" $PFX /bin/sh -c ': > "$1/.iso-canary-in"; : > "$1/../.iso-canary-out"' sh "$APP" > "$OUT/canary.out" 2>&1
      echo "canary: wrote $APP/.iso-canary-in and $APP/../.iso-canary-out" | tee -a "$OUT/steps.txt" ;;
    *) fail "unknown phase $phase" ;;
  esac
done
pgrep -x bots >/dev/null 2>&1 && echo "note: a bots process is still running (not ours to stop if its pid differs from $PID)"

# ---- what appeared ----
: > "$OUT/leaks.txt"
find "$H" "$T" ! -type d 2>/dev/null | sed "s|^$W/run/||" | sed 's|^|SANDBOX |' >> "$OUT/leaks.txt"
# Attributed writes (the trace).
if [ "$OS" = Darwin ]; then
  sleep 3
  /usr/bin/log show --start "$T0" --style compact --predicate 'eventMessage CONTAINS "allow file-write"' 2>/dev/null |
    sed -n 's/.*Sandbox: \([^(]*\)([0-9]*) allow \(file-write[a-z-]*\) \(.*\)$/TRACE \1 \2 \3/p' |
    grep -E "^TRACE (bots[a-z]*|curl|hdiutil|ditto|codesign|open|sh|sleep|python3?|xattr|mount_apfs|diskutil|bash|zsh|dash) " > "$OUT/writes-raw.txt"
  # The log also holds other programs that run under their own report rules; ours are the ones whose path is not in /dev and
  # whose process is bots or something bots started. Keep every process name: the reader sees them.
  awk '{ $1=""; print }' "$OUT/writes-raw.txt" | sort | uniq -c | sort -rn > "$OUT/writes-summary.txt"
  sed 's/^TRACE [^ ]* [^ ]* /TRACE /' "$OUT/writes-raw.txt" | sort -u >> "$OUT/leaks.txt"
elif ls "$OUT"/strace-*.txt >/dev/null 2>&1; then
  # opens for writing, and the calls that create, move or delete something, outside the portable folder and our output
  cat "$OUT"/strace-*.txt | grep -E ' (open|openat|creat|mkdir|mkdirat|rename|renameat|renameat2|unlink|unlinkat|rmdir|symlink|symlinkat|link|linkat|truncate|chmod|fchmodat)\(' |
    grep -v ' = -1 ' | grep -E ' (creat|mkdir|mkdirat|rename|renameat|renameat2|unlink|unlinkat|rmdir|symlink|symlinkat|link|linkat|truncate|chmod|fchmodat)\(|O_WRONLY|O_RDWR|O_CREAT' |
    grep -oE '"/[^"]*"' | tr -d '"' | grep -v "^/dev/\|^/proc/\|^/sys/\|^$APP/\|^$OUT/" |
    sort -u | sed 's|^|TRACE |' >> "$OUT/leaks.txt"
fi
while IFS= read -r d; do
  [ -d "$d" ] && find "$d" -newer "$W/marker" ! -type d 2>/dev/null | grep -v "^$W/" |
    { if [ "$OS" = Darwin ]; then grep -iE 'skullburglar|/bots(iso)?(\.|/|$)|bots-recomp' | grep -v 'bots-review'; else cat; fi; } |
    sed 's|^|REAL |' >> "$OUT/leaks.txt"
done < "$WATCHFILE"
# The canary check: the write above the portable folder must be in the list, the one inside it must not. Both are then
# removed so the real result is not about them (a run that lists the canary above fails when it should not).
if [ -f "$W/run/.iso-canary-out" ] || [ -f "$APP/.iso-canary-in" ]; then
  canary_ok=1
  grep -q "iso-canary-out" "$OUT/leaks.txt" || { echo "FAIL: the harness did not report a write one level above the portable folder"; canary_ok=0; }
  grep -q "iso-canary-in" "$OUT/leaks.txt" && { echo "FAIL: the harness reported a write inside the portable folder"; canary_ok=0; }
  [ "$canary_ok" = 1 ] && echo "canary: PASS (a write above the portable folder is reported, one inside is not)"
  rm -f "$W/run/.iso-canary-out" "$APP/.iso-canary-in"
  grep -v "iso-canary" "$OUT/leaks.txt" > "$OUT/leaks.tmp"; mv "$OUT/leaks.tmp" "$OUT/leaks.txt"
  [ "$canary_ok" = 1 ] || exit 1
fi
# Files inside the portable folder are fine; a crash archive, logs and bots.toml must be there.
echo "portable folder (allowed) now holds:" > "$OUT/portable-folder.txt"
(cd "$APP" && find bots-data ! -type d | sort) >> "$OUT/portable-folder.txt"
# Allow-list: lines "<os> <regex>  # reason"; a leak matching an entry for this OS is accepted and counted.
ALLOW=$HERE/portable_isolation_allow.txt
: > "$OUT/leaks-unexplained.txt"; : > "$OUT/leaks-allowed.txt"
sort -u "$OUT/leaks.txt" | while IFS= read -r line; do
  path=${line#* }
  hit=""
  if [ -f "$ALLOW" ]; then
    while IFS= read -r a; do
      case $a in ''|'#'*) continue ;; esac
      aos=${a%% *}; rest=${a#* }; rx=${rest%%  #*}
      { [ "$aos" = "$OS" ] || [ "$aos" = all ]; } && printf '%s\n' "$path" | grep -Eq -- "$rx" && { hit="$a"; break; }
    done < "$ALLOW"
  fi
  if [ -n "$hit" ]; then echo "$line   [allowed: ${hit#*#}]" >> "$OUT/leaks-allowed.txt"; else echo "$line" >> "$OUT/leaks-unexplained.txt"; fi
done
echo "== sessions =="; cat "$OUT/steps.txt" | grep -E "started|exit status|still running|not reached"
echo "== portable folder: $(grep -c . "$OUT/portable-folder.txt") entries (see $OUT/portable-folder.txt) =="
echo "== allowed (OS-mandated, justified in portable_isolation_allow.txt): $(grep -c . "$OUT/leaks-allowed.txt") =="
sed 's/^/  /' "$OUT/leaks-allowed.txt"
n=$(grep -c . "$OUT/leaks-unexplained.txt")
echo "== LEAKS outside the portable folder: $n =="
sed 's/^/  /' "$OUT/leaks-unexplained.txt"
[ "$n" -eq 0 ] && echo "PASS: nothing written outside the portable folder" && exit 0
echo "FAIL: $n file(s) outside the portable folder"; exit 1
