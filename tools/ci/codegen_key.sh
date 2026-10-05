#!/bin/sh
# Prints the key of the codegen output that this tree and SDK produce: a hash of everything codegen reads.
#   tools/ci/codegen_key.sh <sdk dir>
# The same key means the same generated/ on any machine, so stored output can stand in for running codegen
# (tools/ci/codegen_publish.sh stores it, the release builds restore it). Inputs:
#   - the game executable, by default.xex.sha1 (build.sh checks the file against it)
#   - the manifest and config/
#   - the SDK sources that decide what codegen writes: the recompiler and its CLI, the generated CMake template, the
#     disassembler, and the kernel/system code it loads the executable and resolves imports with. Broader than
#     strictly needed: a change there that doesn't affect the output only costs a codegen run.
set -eu
SDK=$(cd "${1:?usage: tools/ci/codegen_key.sh <sdk dir>}" && pwd)
cd "$(dirname "$0")/../.."
if command -v sha256sum >/dev/null; then SUM=sha256sum; else SUM="shasum -a 256"; fi
{
  echo "codegen key v1"
  $SUM default.xex.sha1 bots_manifest.toml config/*.toml
  (cd "$SDK" && find src/codegen src/rexglue src/kernel src/system include/rex/codegen include/rex/ppc \
     include/rex/kernel include/rex/system resources/templates thirdparty/disasm -type f -print \
     | LC_ALL=C sort | tr '\n' '\0' | xargs -0 $SUM)
} | $SUM | cut -c1-16
