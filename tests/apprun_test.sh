#!/bin/sh
# packaging/linux/AppRun: one log per launch under the state directory, newest 10 kept, other files left alone, and a
# --log_file given by the player still comes last (so it wins). Runs the real script against a stub `bots`.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
mkdir -p "$t/app" "$t/state with space/bots-recomp/logs"
cp "$here/packaging/linux/AppRun" "$t/app/AppRun"
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "%s/args"\n: > "${1#--log_file=}"\n' "$t" > "$t/app/bots"  # logs like the game
chmod +x "$t/app/AppRun" "$t/app/bots"
L="$t/state with space/bots-recomp/logs"
for d in 01 02 03 04 05 06 07 08 09 10 11 12; do echo "session $d" > "$L/bots-202601$d-120000.log"; done
echo "old single log" > "$L/bots.log"
echo "notes" > "$L/mynotes.txt"
XDG_STATE_HOME="$t/state with space" "$t/app/AppRun" --some_flag --log_file=/x/mine.log

fail() { echo "FAIL: $*" >&2; exit 1; }
n=$(ls "$L"/bots-*.log | wc -l | tr -d ' ')
[ "$n" = 10 ] || fail "expected 10 session logs, found $n"
[ ! -e "$L/bots-20260101-120000.log" ] && [ ! -e "$L/bots-20260103-120000.log" ] || fail "oldest logs were kept"
[ -e "$L/bots-20260104-120000.log" ] && [ -e "$L/bots-20260112-120000.log" ] || fail "newer logs were removed"
[ -e "$L/bots.log" ] && [ -e "$L/mynotes.txt" ] || fail "a file that isn't a session log was touched"
first=$(sed -n 1p "$t/args")
case "$first" in
  "--log_file=$L/bots-"[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9].log) ;;
  *) fail "first argument was '$first'" ;;
esac
[ "$(sed -n 2p "$t/args")" = "--some_flag" ] && [ "$(sed -n 3p "$t/args")" = "--log_file=/x/mine.log" ] || fail "player arguments changed"

# Portable mode: no --log_file of ours (the game logs into its data folder), and no state-dir logging.
cd "$t"  # the stub bots creates a file named after its first argument
mkdir -p "$t/ai"
touch "$t/ai/portable.txt"
rm -f "$t/args"
APPIMAGE="$t/ai/Skullburglar.AppImage" XDG_STATE_HOME="$t/state with space" "$t/app/AppRun" --some_flag 2>/dev/null || true
[ "$(cat "$t/args")" = "--some_flag" ] || fail "portable.txt: arguments were '$(cat "$t/args")'"
rm -f "$t/args" "$t/ai/portable.txt"
BOTS_DATA_DIR="$t/d" XDG_STATE_HOME="$t/state with space" "$t/app/AppRun" --some_flag 2>/dev/null || true
[ "$(cat "$t/args")" = "--some_flag" ] || fail "BOTS_DATA_DIR: arguments were '$(cat "$t/args")'"
rm -f "$t/args"
APPIMAGE="$t/ai/Skullburglar.AppImage" XDG_STATE_HOME="$t/state with space" "$t/app/AppRun" --some_flag
case "$(sed -n 1p "$t/args")" in "--log_file="*) ;; *) fail "no portable.txt: --log_file missing" ;; esac
echo "apprun_test: ok"
