#!/bin/sh
# Scripted one-PC co-op: one --coop launch; player 1 hosts a New Story, player 2 (spawned by player 1) picks
# Join COOP and the first game in the list. Prints both logs' co-op / netplay lines at the end.
#   tools/coop_test.sh [secs=160] [tag=coop]      extra bots args in $ARGS; A_INPUT / B_INPUT override the scripts
# Logs logs/<tag>.log + logs/<tag>-p2.log, frames run/shots-<tag>/p1 and p2. Test profiles run/coop/user (player 2:
# run/coop/user-p2), never run/user. Keyboard/mouse is off: a pointer event over a window would move its menu focus.
# Without a save only New Story and Join COOP are enabled, so player 2 reaches Join COOP with UP (it wraps).
set -u
cd "$(dirname "$0")/.."
BIN=${BIN:-$PWD/out/build/mac-arm64-relwithdebinfo/bots}
secs=${1:-160}; tag=${2:-coop}
shots=run/shots-$tag
rm -rf $shots; mkdir -p $shots/p1 $shots/p2 logs
BOTS_INPUT="${A_INPUT:-22:START,26:A,30:A,34:A,38:A,42:A,46:A}" \
  BOTS_P2_INPUT="${B_INPUT:-22:START,26:A,34:UP:200,38:A,44:A,50:A,62:A,68:A,76:X,84:A,90:A,96:A}" \
  BOTS_PRECOMPILE_HOLD=0 BOTS_CAPTURE_DIR=$PWD/$shots/p1 BOTS_P2_CAPTURE_DIR=$PWD/$shots/p2 \
  BOTS_CAPTURE_SECS=${CAP:-4} \
  perl -e "alarm $secs; exec @ARGV" "$BIN" --game_data_root=game --user_data_root=run/coop/user \
  --cache_root=run/coop/cache --coop=true --kbm=false --netplay_trace=true --log_level=info \
  --log_file=logs/$tag.log ${ARGS:-} > logs/$tag.out 2>&1
pkill -f "coop_player=2"  # player 2 outlives the alarm on player 1
for f in $tag $tag-p2; do
  echo "== $f"
  grep -hE "coop:|input: pad|B-057|online:|netplay: slot|Unhandled|FATAL" logs/$f.log \
    | sed -E 's/^\[[^ ]* ([^]]*)\][^]]*\][^]]*\][^]]*\] /\1 /' | cut -c1-150 | head -${N:-20}
done
echo "frames: p1 $(ls $shots/p1 | wc -l), p2 $(ls $shots/p2 | wc -l)"
