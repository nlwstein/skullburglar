#!/bin/sh
# Visit many disc checkpoints in ONE launch: the game's own `LoadChapter <id>` console command
# (C1PlayerController exec → ClientTravel "FiftyCentTwo_G?Listen?loadchapter=<id>"), sent through
# BOTS_EXEC_FILE every <secs>. Coverage/smoke test for every area of the game (B-039).
#   tools/chapter_tour.sh [secs=45] [chapter ids...]   (default: every game/C1Game/Checkpoints/chapter*.sav)
# Env: TOUR_START (seconds before the first LoadChapter, default 45: title screen is up by then),
#      TOUR_INPUT (BOTS_INPUT script, default: press START at 22 s to leave the attract screen).
# Output: run/tour/ (frames every 5 s, schedule.txt), log logs/run-N.log; then
#   uv run --with pillow python tools/tour_sheet.py logs/run-N.log run/tour/sheet.jpg
cd "$(dirname "$0")/.."
secs=${1:-45}; [ $# -gt 0 ] && shift
ids=${*:-$(ls game/C1Game/Checkpoints | sed 's/chapter//;s/\.sav//' | sort -n)}
start=${TOUR_START:-45}
n=$(echo $ids | wc -w)
rm -rf run/tour; mkdir -p run/tour
: > run/tour/exec.txt
(
  sleep "$start"
  for id in $ids; do
    echo "LoadChapter $id" >> run/tour/exec.txt
    echo "$(date +%s) $id" >> run/tour/schedule.txt
    sleep "$secs"
  done
) &
writer=$!
BOTS_EXEC_FILE="$PWD/run/tour/exec.txt" BOTS_INVULNERABLE=1 BOTS_INPUT="${TOUR_INPUT:-22:START}" \
  REX_FPS_LOG=1 BOTS_CAPTURE_DIR="$PWD/run/tour" BOTS_CAPTURE_SECS=5 \
  tools/run.sh $((start + n * secs + 20)) | grep "^exit="
kill $writer 2>/dev/null
echo "chapters sent: $(wc -l < run/tour/schedule.txt) of $n"
