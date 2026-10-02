#!/bin/sh
# Sample the bots process for 10 s once level-1 gameplay is running (B-032).
#   tools/profile_gameplay.sh [at_secs=155]  → logs/sample-N.txt (+ run log)
cd "$(dirname "$0")/.."
at=${1:-155}
n=1; while [ -e "logs/sample-$n.txt" ]; do n=$((n+1)); done
REX_FPS_LOG=1 BOTS_CAPTURE_DIR=${BOTS_CAPTURE_DIR:-} BOTS_INPUT="22:START,30:A,36:A,42:A,48:A,54:A,60:A,145:A,148:B,152:LUP:8000" \
  tools/run.sh $((at + 20)) > /dev/null 2>&1 &
sleep "$at"
pid=$(pgrep -f "bots --game_data_root" | head -1)
sample "$pid" 10 -file "$PWD/logs/sample-$n.txt" > "logs/sample-$n.err" 2>&1
wait
echo "logs/sample-$n.txt"
