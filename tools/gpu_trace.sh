#!/bin/sh
# Metal System Trace of level-1 gameplay (B-032): drives the pad to gameplay, then attaches
# xctrace for N seconds.  tools/gpu_trace.sh [at=155] [secs=3] → logs/gpu-N.trace
# Inspect: xcrun xctrace export --input logs/gpu-N.trace --toc
cd "$(dirname "$0")/.."
at=${1:-155}; secs=${2:-3}
n=1; while [ -e "logs/gpu-$n.trace" ]; do n=$((n+1)); done
REX_FPS_LOG=1 BOTS_INPUT="22:START,30:A,36:A,42:A,48:A,54:A,60:A,145:A,148:B,152:LUP:8000" \
  tools/run.sh $((at + secs + 30)) > /dev/null 2>&1 &
sleep "$at"
pid=$(pgrep -f "bots --game_data_root" | head -1)
xcrun xctrace record --template 'Metal System Trace' --attach "$pid" --time-limit "${secs}s" \
  --output "$PWD/logs/gpu-$n.trace" > "logs/gpu-$n.err" 2>&1
wait
echo "logs/gpu-$n.trace"
