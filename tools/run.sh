#!/bin/sh
# Launch the recompiled game with a timeout and a numbered log.
#   tools/run.sh [seconds=90] [extra bots args...]
# Log: logs/run-N.log (next free N). Prints the last lines + exit status.
set -u
cd "$(dirname "$0")/.."
secs=${1:-90}; [ $# -gt 0 ] && shift
n=1; while [ -e "logs/run-$n.log" ]; do n=$((n+1)); done
log="logs/run-$n.log"
mkdir -p run/user run/cache logs
perl -e "alarm $secs; exec @ARGV" ./out/build/mac-arm64-relwithdebinfo/bots \
  --game_data_root=game --user_data_root=run/user --cache_root=run/cache \
  --gpu_plugin=xenos --log_file="$log" --log_level=info "$@" > "logs/run-$n.out" 2>&1
rc=$?
echo "exit=$rc ($( [ $rc -eq 142 ] && echo "timeout ${secs}s" || echo exited)) log=$log lines=$(wc -l < "$log")"
grep -nE "critical|FATAL|error" "$log" | head -5
tail -5 "$log"
