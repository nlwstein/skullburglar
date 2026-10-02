#!/bin/sh
# Codegen → label check → full build, with numbered logs. Stops before compiling if the
# label check finds cross-function gotos (fix those with `end` overrides first).
#   tools/rebuild.sh          (FORCE=1 to regenerate even if the codegen stamp matches —
#                              needed after rebuilding the SDK, which the stamp ignores)
set -u
cd "$(dirname "$0")/.."
n=1; while [ -e "logs/build-$n.log" ]; do n=$((n+1)); done
B=out/build/mac-arm64-relwithdebinfo
start=$(date +%s)
if [ "${FORCE:-0}" = 1 ]; then
  "${SDK:-$HOME/repos/bots-recomp-ext/rexglue-sdk}/out/install/mac-arm64/bin/rexglue" codegen --ignore-stamp bots_manifest.toml > "logs/codegen-$n.log" 2>&1
else
  cmake --build $B --target bots_codegen > "logs/codegen-$n.log" 2>&1
fi || { echo "codegen FAILED (logs/codegen-$n.log)"; exit 1; }
echo "codegen ok $(( $(date +%s)-start ))s; unresolved=$(grep -c Unresolved "logs/codegen-$n.log")"
python3 tools/check_labels.py generated/default > logs/missing_labels.tsv
[ -s logs/missing_labels.tsv ] && { echo "missing labels:"; cat logs/missing_labels.tsv; exit 1; }
cmake --build $B --parallel 6 -- -k 0 > "logs/build-$n.log" 2>&1
rc=$?
echo "build exit=$rc total $(( $(date +%s)-start ))s (logs/build-$n.log)"
[ $rc -ne 0 ] && grep -E "error:|Undefined symbols|\"_sub_" "logs/build-$n.log" | sort | uniq -c | head -20
exit $rc
