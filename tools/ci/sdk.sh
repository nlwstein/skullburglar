#!/bin/sh
# Check out the pinned ReXGlue SDK and apply this repo's patch series (patches/rexglue-sdk) to it.
#   tools/ci/sdk.sh <dir>
# A fresh clone each time: release builds compile the SDK from source next to the game (REXSDK_DIR), so the
# binaries always match the patches committed here. Shallow, with submodules (SUBMODULES=0: without, enough to
# check that the patches apply).
set -eu
DIR=${1:?usage: tools/ci/sdk.sh <dir>}
TAG=v0.10.0
REPO=$(cd "$(dirname "$0")/../.." && pwd)

if [ -e "$DIR" ]; then echo "$DIR already exists" >&2; exit 1; fi
set --
[ "${SUBMODULES:-1}" = 0 ] || set -- --recurse-submodules --shallow-submodules
git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" "$@" \
  https://github.com/rexglue/rexglue-sdk.git "$DIR"
for p in "$REPO"/patches/rexglue-sdk/*.patch; do
  git -C "$DIR" apply --whitespace=nowarn "$p"
done
echo "rexglue-sdk $TAG + $(ls "$REPO"/patches/rexglue-sdk/*.patch | wc -l | tr -d ' ') patches in $DIR"
