#!/bin/sh
# Boot straight into each persistent map (BOTS_CMDLINE launch data) with random play, and
# summarize crash/stub/fps per map (B-039).
#   tools/map_sweep.sh [secs=240] [maps...]  → logs/run-N.log per map, frames in run/sweep-<map>/
cd "$(dirname "$0")/.."
secs=${1:-240}; [ $# -gt 0 ] && shift
maps=${*:-"Ambush_P Crib_P DesertChase_P Helicopter_P MountainCity_P TheFlyover_P TheMall_P TheTheatre_P"}
for map in $maps; do
  d=$(echo "$map" | tr "?=" "__"); rm -rf "run/sweep-$d"
  out=$(BOTS_CMDLINE="$map" BOTS_INPUT_RANDOM=${SWEEP_RANDOM-40:5} REX_FPS_LOG=1 BOTS_CAPTURE_DIR="$PWD/run/sweep-$d" \
        BOTS_CAPTURE_SECS=15 tools/run.sh "$secs" 2>&1 | grep "^exit=")
  log=$(echo "$out" | sed -E 's/.*log=([^ ]+).*/\1/')
  base=${log%.log}
  fps=$(grep -h "guest fps" "$base".*log "$log" 2>/dev/null | sed -E 's/.*guest fps ([0-9.]+).*/\1/' | tail -n +6 |
        sort -n | awk '{a[NR]=$1} END{if(NR) printf "min %s med %s max %s", a[1], a[int((NR+1)/2)], a[NR]}')
  bad=$(grep -hE "FATAL| STUB|Unimplemented" "$base".*log "$log" 2>/dev/null | sort | uniq -c | sort -rn | head -3 | tr '\n' ';')
  echo "$map | $out | fps $fps | $bad"
done
