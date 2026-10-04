#!/bin/sh
# Native Linux (x86_64, Vulkan) build in a throwaway Docker container (image: docker/linux/Dockerfile).
# Run after codegen (generated/ must be current):  tools/linux/build.sh
# Builds with this machine's Docker by default; BUILD_HOST=user@host builds on a remote Docker host over SSH.
# Nothing is installed on the build machine: everything lives in ~/bots/linux and the `bots-linux` container
# (LINUX_NAME=x uses ~/bots/x and container bots-x instead, so parallel work doesn't share a build).
# Output: ~/bots/linux/build/bots (+ SDK .so files) on the build machine.
#   SDK=<dir>   rexglue-sdk checkout (default ~/repos/bots-recomp-ext/rexglue-sdk)
#   GEN=<dir>   generated/ to use (default ./generated; a worktree has none, so point it at the main checkout's)
#   CPUS=<n>    container CPU cap and build parallelism (default: all cores of the build machine)
#   CMAKE_ARGS  extra -D settings for the build dir (e.g. "-DBOTS_PROFILING=ON" for Tracy + SDK perf counters)
#   IMAGE=steamrt  build in the Steam Runtime 3 (sniper) image (docker/linux-steamrt): glibc 2.31, static libstdc++,
#               so the binaries run on the Steam Deck and other distros as they are. Use its own LINUX_NAME.
# The game itself is mounted read-only from ~/bots/game on the build machine (used by runs, not the build).
set -eu
cd "$(dirname "$0")/../.."
H=${BUILD_HOST:-}
SDK=${SDK:-$HOME/repos/bots-recomp-ext/rexglue-sdk}
GEN=${GEN:-generated}
N=${LINUX_NAME:-linux}
C=bots-$N
case ${IMAGE:-} in
  "") D=docker/linux; I=bots-linux ;;
  steamrt) D=docker/linux-steamrt; I=bots-linux-steamrt ;;
  *) echo "unknown IMAGE=$IMAGE" >&2; exit 1 ;;
esac
on_host() { if [ -n "$H" ]; then ssh "$H" "$1"; else sh -c "$1"; fi; }
dest() { if [ -n "$H" ]; then echo "$H:$1"; else echo "$HOME/$1"; fi; }
[ -f "$GEN/rexglue.cmake" ] || { echo "no codegen output in $GEN (run codegen, or set GEN)" >&2; exit 1; }

on_host "mkdir -p ~/bots/$N/image"
# By content, without copying mtimes: a file that changed gets a new mtime, so ninja rebuilds what depends on it even
# when the source copy's mtime is older than the objects (e.g. after switching SDK=, the old -a sync left objects built
# against the other SDK's headers in place, with a different class layout than the rest of the build).
SYNC="rsync -rlpz --checksum --delete"
$SYNC --exclude out/ --exclude .git/ "$SDK/" "$(dest bots/$N/sdk/)"
$SYNC --exclude out/ --exclude .git/ --exclude .claude/ --exclude game/ --exclude logs/ \
  --exclude run/ --exclude dumps/ --exclude docs/ --exclude ops/ --exclude shaders/ --exclude generated/ \
  ./ "$(dest bots/$N/game/)"
$SYNC "$GEN/" "$(dest bots/$N/game/generated/)"
rsync -a $D/Dockerfile "$(dest bots/$N/image/)"

on_host "CPUS=${CPUS:-} N=$N C=$C I=$I CMAKE_ARGS='${CMAKE_ARGS:-}'"'
set -e
c=${CPUS:-$(nproc)}
cd ~/bots/$N
docker build -q -t $I image >/dev/null
# Guest memory is POSIX shm (SDK memory_posix.cpp): the Docker default 64 MB /dev/shm = SIGBUS at boot.
if ! docker ps --format "{{.Names}}" | grep -qx $C; then
  docker rm -f $C 2>/dev/null || true
  docker run -d --name $C --cpus $c --memory 40g --shm-size 8g --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all \
    -v ~/bots/$N:/w -v ~/bots/game:/g:ro $I sleep infinity
fi
# x86-64-v2 (SSSE3/SSE4) is the SDK minimum, as in its presets: reconfigure a build dir made without it or a higher
# level (build/ is written by root in the container, so clean it from there).
grep -qs "march=x86-64-v[234]" build/CMakeCache.txt || docker exec $C rm -rf /w/build
# Sources are pre-generated on the host that ran codegen (same as the Windows build): make the codegen step a no-op.
sed -i "s|    COMMAND \$<TARGET_FILE:rex::rexglue> codegen \${CMAKE_CURRENT_SOURCE_DIR}/bots_manifest.toml|    COMMAND \${CMAKE_COMMAND} -E true|" game/generated/rexglue.cmake
sed -i "/DEPFILE .*codegen.d/d" game/generated/rexglue.cmake
docker exec -w /w/game $C sh -c "[ -f /w/build/build.ninja ] || cmake -G Ninja -B /w/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DREXSDK_DIR=/w/sdk -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
# Extra cache settings (CMAKE_ARGS) are applied to the existing build dir too.
[ -z "$CMAKE_ARGS" ] || docker exec -w /w/game $C cmake -B /w/build $CMAKE_ARGS >/dev/null
docker exec $C cmake --build /w/build --parallel $c
docker exec $C sh -c "ls -la /w/build/bots"
'
