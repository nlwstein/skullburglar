#!/bin/sh
# Names the host function and source line of a crash printed in a run's console output (Linux and macOS builds).
#   tools/crash_where.sh <bots executable with debug info> <run.out> [more run.out ...]
# The console shows "=== unhandled host signal N at pc 0x..., fault address 0x... ===" (a host fault the game could not
# handle; guest code is recompiled to host functions named __imp__sub_<guest address>) and "=== bots crash: signal N,
# load addr 0x... ===" (the load address of the executable in that process). This subtracts the second from the first and
# asks gdb (or llvm-symbolizer) which function and line that is, so a crash seen once can be pinned to a guest function
# without a core dump. The executable must be the unstripped build (CI ships the debug info separately in `symbols/`).
set -eu
EXE=${1:?usage: tools/crash_where.sh <bots executable> <run.out> ...}
shift
for f in "$@"; do
  load=$(sed -n 's/^=== bots crash: signal [0-9]*, load addr \(0x[0-9a-f]*\) ===$/\1/p' "$f" | head -n 1)
  sed -n 's/^=== unhandled host signal \([0-9]*\) at pc \(0x[0-9a-f]*\), fault address \(0x[0-9a-f]*\) ===$/\1 \2 \3/p' "$f" |
  sort -u | while read -r sig pc addr; do
    if [ -z "$load" ]; then echo "$f: signal $sig pc $pc fault $addr (no load address line, cannot symbolize)"; continue; fi
    off=$(printf '0x%x' $((pc - load)))
    echo "$f: signal $sig, fault address $addr, pc $pc = executable offset $off"
    if command -v gdb >/dev/null; then
      gdb -batch -ex "info line *$off" "$EXE" 2>&1 | sed -n 's/^Line \([0-9]*\) of "\(.*\)" starts at address.* <\(.*\)> and ends.*/  \3  \2:\1/p'
    elif command -v llvm-symbolizer >/dev/null; then
      llvm-symbolizer --obj="$EXE" --relative-address -f "$off" | sed 's/^/  /'
    else
      echo "  (needs gdb or llvm-symbolizer)"
    fi
  done
done
