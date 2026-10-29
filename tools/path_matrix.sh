#!/bin/sh
# One cell of the folder-name matrix: does the game work when its folders have unusual names (non-ASCII, spaces,
# invalid UTF-8 bytes, long paths)? Two real runs, no clicking:
#   tools/path_matrix.sh <bots executable> <test root> <folder that holds game/> [seconds per run]
# <test root> is a throwaway folder (it gets user/, cache/, logs/); the extracted game must sit at
# <folder that holds game/>/game. Run 1 starts the launcher in auto mode with BOTS_SETUP_FOLDER (the "choose another
# folder" path), so it saves bots.toml and remembers the game folder; run 2 starts from that bots.toml alone (no
# folder on the command line), so settings and the game folder are read back. Both load a chapter (BOTS_EXEC_FILE).
# DEFAULT_DIR=1: <folder that holds game/> is the per-user default (bots.toml then names no folder).
# Prints PASS/FAIL lines and exits 1 on any FAIL. Put LD_LIBRARY_PATH, DISPLAY, LANG/LC_ALL ... in the environment
# first; EXTRA_ARGS adds bots arguments. Uses its own user data root, never a player's save.
set -u
EXE=${1:?usage: path_matrix.sh <bots executable> <test root> <folder that holds game/> [seconds]}
ROOT=${2:?test root}
GP=${3:?folder that holds game/}
SECS=${4:-110}
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
check() { # label, then a command
  l=$1; shift
  if "$@"; then pass "$l"; else fail "$l"; fi
}
nogrep() { ! grep -q -- "${2:-unreadable\|Failed to parse}" "$1" 2>/dev/null; }
has() { grep -qF -- "$2" "$1" 2>/dev/null; }

mkdir -p "$ROOT/user" "$ROOT/logs"
: > "$ROOT/exec.txt"
# A setting the player might have changed: it has to survive both runs.
printf 'framerate_limit = 30\n' > "$ROOT/user/bots.toml"

run() { # n [VAR=value ...]
  n=$1; shift
  : > "$ROOT/exec.txt"
  (
    exec env BOTS_PRECOMPILE_HOLD=0 BOTS_INVULNERABLE=1 BOTS_INPUT=22:START BOTS_EXEC_FILE="$ROOT/exec.txt" \
      BOTS_CAPTURE_DIR="$ROOT/shots$n" BOTS_CAPTURE_SECS=${CAPTURE_SECS:-20} "$@" \
      perl -e "alarm $SECS; exec @ARGV" "$EXE" --user_data_root="$ROOT/user" --cache_root="$ROOT/cache" \
      --log_file="$ROOT/logs/r$n.log" --fullscreen=false --window_width=1280 --window_height=720 ${EXTRA_ARGS:-} \
      > "$ROOT/r$n.out" 2>&1
  ) &
  pid=$!
  sleep 45
  echo "LoadChapter 11010" >> "$ROOT/exec.txt"
  wait $pid
  echo "exit=$?" >> "$ROOT/r$n.out"
}

GAME="$GP/game"
run 1 BOTS_LAUNCHER=1 BOTS_SETUP_AUTO=1 BOTS_SETUP_FOLDER="$GP"
L1=$ROOT/logs/r1.log
check "run 1: log file written in the test root" test -s "$L1"
check "run 1: launcher found the game in the chosen folder" has "$L1" "game files installed at $GAME"
check "run 1: settings saved" has "$L1" "setup: settings saved to $ROOT/user/bots.toml"
check "run 1: bots.toml readable and kept the seeded setting" has "$ROOT/user/bots.toml" "framerate_limit = 30"
if [ -n "${DEFAULT_DIR:-}" ]; then  # the folder is the per-user default: bots.toml names no folder
  check "run 1: default folder not written to bots.toml" nogrep "$ROOT/user/bots.toml" game_data_root
elif [ -n "${EXPECT_KEY:-}" ]; then
  check "run 1: game folder remembered ($EXPECT_KEY)" has "$ROOT/user/bots.toml" "$EXPECT_KEY"
else
  check "run 1: game folder remembered in bots.toml" grep -q '^game_data_root' "$ROOT/user/bots.toml"
fi
check "run 1: chapter command ran" has "$L1" "LoadChapter 11010"
check "run 1: n1 cache written" test -s "$ROOT/cache/n1/index.json"
check "run 1: pipeline cache written" test -s "$ROOT/cache/native/5451083B.manifest.bin"
check "run 1: no settings-read errors" nogrep "$L1"
check "run 1: game kept running to the alarm" has "$ROOT/r1.out" "exit=142"

run 2 BOTS_LAUNCHER=1 BOTS_SETUP_AUTO=1
L2=$ROOT/logs/r2.log
check "run 2: game folder read back from bots.toml" has "$L2" "game files installed at $GAME"
check "run 2: chapter command ran" has "$L2" "LoadChapter 11010"
check "run 2: bots.toml still has the setting" has "$ROOT/user/bots.toml" "framerate_limit = 30"
check "run 2: no settings-read errors" nogrep "$L2"
check "run 2: n1 cache still there" test -s "$ROOT/cache/n1/index.json"
echo "files: user $(find "$ROOT/user" -type f | wc -l | tr -d ' '), cache $(find "$ROOT/cache" -type f | wc -l | tr -d ' '), shots $(find "$ROOT"/shots* -type f 2>/dev/null | wc -l | tr -d ' ')"
[ "$fails" -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
[ "$fails" -eq 0 ]
