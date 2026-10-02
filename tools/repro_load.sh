#!/bin/sh
# Repeat the title → new story → level-1 load sequence N times; stop at the first crash.
# Used for B-036 (intermittent FATAL during level load, ~50% of loads before SDK patch 0001).
#   tools/repro_load.sh [N=8] [secs=80]
cd "$(dirname "$0")/.."
N=${1:-8}; secs=${2:-80}; i=1; ok=0
while [ $i -le $N ]; do
  out=$(BOTS_INPUT="22:START,30:A,36:A,42:A,48:A,54:A,60:A" tools/run.sh $secs 2>&1 | grep "^exit")
  echo "try $i: $out"
  case "$out" in *exited*) echo "CRASH after $ok clean"; exit 1;; esac
  ok=$((ok+1)); i=$((i+1))
done
echo "$ok/$N clean"
