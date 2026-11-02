#!/bin/sh
# Repeat the chapter load many times and count crashes (for intermittent load-time crashes such as B-288).
#   tools/load_loop.sh <bots executable> <test root> <folder that holds game/> <runs> [seconds per run] [chapter]
# Each run starts the game with the launcher in auto mode (like tools/path_matrix.sh), waits LOAD_AT seconds (default 45)
# and sends "LoadChapter <chapter>" (default 11010) through BOTS_EXEC_FILE, then lets the game run until the alarm
# (default 90 s). A run that ends by the alarm (exit 142) is clean unless its log has been silent for the last 20 s
# (a hang); anything else is a crash and its console output is
# kept as <test root>/loop/crash-N.out, with the host function named by tools/crash_where.sh when gdb is installed.
#   COLD=1     wipe the pipeline/n1 caches before every run (the default keeps them after the first run)
#   LOAD_AT=n  seconds before the chapter command
#   EXTRA_ARGS bots arguments;  anything else in the environment (DISPLAY, LD_LIBRARY_PATH, BOTS_CAPTURE_DIR ...) is passed on
# Prints one line per run and a summary "crashes: K of N". Uses <test root>/user as the user data root, never a real save.
set -u
EXE=${1:?usage: tools/load_loop.sh <bots executable> <test root> <folder that holds game/> <runs> [seconds] [chapter]}
ROOT=${2:?test root}
GP=${3:?folder that holds game/}
RUNS=${4:?runs}
SECS=${5:-90}
CH=${6:-11010}
LOAD_AT=${LOAD_AT:-45}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$ROOT/user" "$ROOT/loop"
printf 'framerate_limit = 30\n' > "$ROOT/user/bots.toml"
crashes=0
hangs=0
n=1
while [ "$n" -le "$RUNS" ]; do
  [ -z "${COLD:-}" ] || rm -rf "$ROOT/cache"
  # A killed game leaves its guest memory file in /dev/shm (Linux); a few of them and the next run dies with SIGBUS.
  ps -eo stat=,comm= | awk '$2 == "bots" && $1 !~ /^Z/' | grep -q . || rm -f /dev/shm/xenia_memory_* 2>/dev/null
  d=$ROOT/loop/run-$n
  rm -rf "$d"; mkdir -p "$d"
  : > "$d/exec.txt"
  (
    exec env BOTS_PRECOMPILE_HOLD=0 BOTS_INVULNERABLE=1 BOTS_INPUT=22:START BOTS_EXEC_FILE="$d/exec.txt" \
      BOTS_LAUNCHER=1 BOTS_SETUP_AUTO=1 BOTS_SETUP_FOLDER="$GP" \
      perl -e "alarm $SECS; exec @ARGV" "$EXE" --user_data_root="$ROOT/user" --cache_root="$ROOT/cache" \
      --log_file="$d/run.log" --fullscreen=false --window_width=1280 --window_height=720 ${EXTRA_ARGS:-} \
      > "$d/run.out" 2>&1
  ) &
  pid=$!
  sleep "$LOAD_AT"
  echo "LoadChapter $CH" >> "$d/exec.txt"
  wait "$pid"
  rc=$?
  idle=$(perl -e 'print time - (stat shift)[9]' "$d/run.log" 2>/dev/null || echo 0)
  if [ "$rc" -eq 142 ] && [ "$idle" -gt 20 ]; then
    hangs=$((hangs + 1))
    echo "run $n: HANG (alarm, but the log has been silent for $idle s)"
  elif [ "$rc" -eq 142 ]; then
    echo "run $n: clean (alarm)"
  else
    crashes=$((crashes + 1))
    cp "$d/run.out" "$ROOT/loop/crash-$n.out"
    echo "run $n: CRASH exit=$rc ($ROOT/loop/crash-$n.out)"
    if command -v gdb >/dev/null; then "$HERE/crash_where.sh" "$EXE" "$d/run.out" | sed 's/^/    /'; fi
  fi
  n=$((n + 1))
done
echo "crashes: $crashes of $RUNS, hangs: $hangs"
[ "$crashes" -eq 0 ] && [ "$hangs" -eq 0 ]
