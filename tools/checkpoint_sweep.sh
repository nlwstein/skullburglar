#!/bin/sh
# Start from disc checkpoints through the real menu path (Continue Story) instead of a map warp,
# with random play; summarize crash/stub/fps per checkpoint (B-040).
#   tools/checkpoint_sweep.sh [secs=240] [chapter ids...]  → run/cp-<id>/ frames, logs/run-N.log
# Env: SWEEP_RANDOM=<start>:<seed> (empty = none), CP_INPUT=<script> (default: menus → Continue Story),
#      CP_CAPTURE_SECS (15).
# The disc's chapter<N>.sav is turned into a user save: dword 0 (2 on disc → 0) and the 16-bit field
# 8 bytes from the end (0x0320 on disc → 0) are zeroed, matching what the game itself writes; with
# the disc values the save is ignored and Continue Story falls through to New Story (run 72).
# The current user save is backed up to run/save-before-checkpoint-sweep.bin and restored at the end.
cd "$(dirname "$0")/.."
secs=${1:-240}; [ $# -gt 0 ] && shift
ids=${*:-"41270 51000 61465 71515 81610 91710"}
S=run/user/B13EBABEBABEBABE/5451083B/00000001/default_checkpoint.sav/Use
[ -f "$S" ] && cp "$S" run/save-before-checkpoint-sweep.bin
for id in $ids; do
  python3 - "game/C1Game/Checkpoints/chapter$id.sav" "$S" <<'PY'
import sys
b = bytearray(open(sys.argv[1], "rb").read())
b[0:4] = bytes(4); b[-8:-6] = bytes(2)
open(sys.argv[2], "wb").write(b)
PY
  rm -rf "run/cp-$id"
  out=$(BOTS_INPUT="${CP_INPUT:-22:START,30:A,36:DOWN,38:A,44:A,50:A,56:A}" BOTS_INPUT_RANDOM=${SWEEP_RANDOM-90:5} \
        REX_FPS_LOG=1 BOTS_CAPTURE_DIR="$PWD/run/cp-$id" BOTS_CAPTURE_SECS=${CP_CAPTURE_SECS:-15} tools/run.sh "$secs" 2>&1 | grep "^exit=")
  log=$(echo "$out" | sed -E 's/.*log=([^ ]+).*/\1/'); base=${log%.log}
  fps=$(grep -h "guest fps" "$base".*log "$log" 2>/dev/null | sed -E 's/.*guest fps ([0-9.]+).*/\1/' | tail -n +12 |
        sort -n | awk '{a[NR]=$1} END{if(NR) printf "min %s med %s max %s", a[1], a[int((NR+1)/2)], a[NR]}')
  bad=$(grep -hE "FATAL| STUB|Unimplemented|access violation" "$base".*log "$log" 2>/dev/null | sort | uniq -c | sort -rn | head -3 | tr '\n' ';')
  echo "$id | $out | fps $fps | $bad"
done
[ -f run/save-before-checkpoint-sweep.bin ] && cp run/save-before-checkpoint-sweep.bin "$S"
