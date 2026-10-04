#!/bin/sh
# Cross-compile the Windows (x86_64, D3D12) build in a throwaway Docker container.
# Run after codegen (generated/ must be current):  tools/win-cross/build.sh
# Builds with this machine's Docker by default; BUILD_HOST=user@host builds on a remote Docker host over SSH.
# Nothing is installed on the build machine: everything lives in ~/bots/win and the `bots-win` container
# (ubuntu:26.04 + clang/lld/cmake/ninja + xwin's MSVC CRT / Windows SDK in ~/bots/win/xwin).
# Output: ~/bots/win/dist/bots-win64.zip on the build machine.
#   SDK=<dir>   rexglue-sdk checkout (default ~/repos/bots-recomp-ext/rexglue-sdk)
#   GEN=<dir>   generated/ to use (default ./generated; a worktree has none, so point it at the main checkout's)
#   CPUS=<n>    container CPU cap and build parallelism (default: all cores of the build machine)
#   WIN_NAME=x  use ~/bots/x and container bots-x instead of ~/bots/win and bots-win (parallel builds; the
#               MSVC CRT / Windows SDK in ~/bots/win/xwin is mounted read-only when it exists)
#   CMAKE_ARGS  extra configure arguments, e.g. "-DREXGLUE_USE_VULKAN=ON -DBOTS_NATIVE_GFX=ON" for the native
#               renderer (Vulkan, next to D3D12). They apply on every run: configure is re-run when they are set.
set -eu
cd "$(dirname "$0")/../.."
H=${BUILD_HOST:-}
SDK=${SDK:-$HOME/repos/bots-recomp-ext/rexglue-sdk}
GEN=${GEN:-generated}
N=${WIN_NAME:-win}
on_host() { if [ -n "$H" ]; then ssh "$H" "$1"; else sh -c "$1"; fi; }
dest() { if [ -n "$H" ]; then echo "$H:$1"; else echo "$HOME/$1"; fi; }
[ -f "$GEN/rexglue.cmake" ] || { echo "no codegen output in $GEN (run codegen, or set GEN)" >&2; exit 1; }

on_host "mkdir -p ~/bots/$N"
# By content, without copying mtimes, so ninja sees every changed file as new (tools/linux/build.sh has the story).
SYNC="rsync -rlpz --checksum --delete"
$SYNC --exclude out/ --exclude .git/ "$SDK/" "$(dest bots/$N/sdk/)"
$SYNC --exclude out/ --exclude .git/ --exclude .claude/ --exclude game/ --exclude logs/ \
  --exclude run/ --exclude dumps/ --exclude docs/ --exclude ops/ --exclude shaders/ --exclude generated/ \
  ./ "$(dest bots/$N/game/)"
$SYNC "$GEN/" "$(dest bots/$N/game/generated/)"
rsync -a tools/win-cross/win-clang.cmake tools/win-cross/casefix.py "$(dest bots/$N/)"

on_host "CPUS=${CPUS:-} N=$N CMAKE_ARGS='${CMAKE_ARGS:-}'"'
set -e
c=${CPUS:-$(nproc)}
C=bots-$N
if ! docker ps --format "{{.Names}}" | grep -qx $C; then
  docker rm -f $C 2>/dev/null || true
  x=; [ $N = win ] || [ ! -d ~/bots/win/xwin/sdk ] || x="-v $HOME/bots/win/xwin:/w/xwin:ro"
  docker run -d --name $C --cpus $c --memory 40g -v ~/bots/$N:/w $x ubuntu:26.04 sleep infinity
  docker exec $C sh -c "apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    clang lld llvm cmake ninja-build curl ca-certificates python3 zip >/dev/null"
fi
docker exec $C sh -c "command -v xwin >/dev/null || (cd /tmp && curl -sL \
  https://github.com/Jake-Shadle/xwin/releases/download/0.6.6/xwin-0.6.6-x86_64-unknown-linux-musl.tar.gz \
  | tar xz && cp xwin-*/xwin /usr/local/bin/)"
docker exec $C sh -c "[ -d /w/xwin/sdk ] || xwin --accept-license splat --output /w/xwin"
docker exec $C python3 /w/casefix.py /w/sdk /w/game/src
# Sources are pre-generated on the host that ran codegen; the Windows rexglue.exe cannot run here.
cd ~/bots/$N/game
sed -i "s|    COMMAND \$<TARGET_FILE:rex::rexglue> codegen \${CMAKE_CURRENT_SOURCE_DIR}/bots_manifest.toml|    COMMAND \${CMAKE_COMMAND} -E true|" generated/rexglue.cmake
sed -i "/DEPFILE .*codegen.d/d" generated/rexglue.cmake
docker exec -w /w/game $C sh -c "[ -f /w/build/build.ninja ] && [ -z \"$CMAKE_ARGS\" ] || cmake -G Ninja -B /w/build \
  -DCMAKE_TOOLCHAIN_FILE=/w/win-clang.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DREXSDK_DIR=/w/sdk $CMAKE_ARGS"
docker exec $C cmake --build /w/build --parallel $c
docker exec $C sh -c "rm -rf /w/dist && mkdir -p /w/dist/bots && cp /w/build/bots.exe /w/dist/bots/ \
  && cp /w/sdk/out/win-amd64/*.dll /w/dist/bots/ 2>/dev/null; cp /w/build/rexgpu-native*.dll /w/dist/bots/ 2>/dev/null; cd /w/dist && zip -qr bots-win64.zip bots && ls -la /w/dist/bots"
'
