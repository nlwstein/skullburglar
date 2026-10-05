#!/bin/sh
# Stores this tree's codegen output in the assets project's package registry, where release builds pick it up instead
# of running codegen (which takes over half an hour on the CI runner).
#   GITLAB_TOKEN=<token with api scope> tools/ci/codegen_publish.sh
# Run it after changing anything codegen reads (tools/ci/codegen_key.sh): the manifest, config/, or an SDK patch to the
# recompiler. Without it, release builds still work: they run codegen themselves.
# Works on a copy of the tree with a fresh SDK from tools/ci/sdk.sh (both in out/codegen-publish), so the output is what
# a release build would generate; generated/ here is left alone. Codegen output is the same on every platform.
# Needs game/default.xex and the build tools of tools/ci/build.sh for this machine (macOS or Linux).
#   ASSETS_PROJECT  the assets project path (default: <this project's namespace>/skullburglar-assets)
set -eu
: "${GITLAB_TOKEN:?set GITLAB_TOKEN (api scope)}"
cd "$(dirname "$0")/../.."
REPO=$PWD
case $(uname -s) in Darwin) PLATFORM=macos ;; Linux) PLATFORM=linux ;; *) echo "macOS or Linux only" >&2; exit 1 ;; esac

# The GitLab instance and namespace this checkout comes from.
ORIGIN=$(git remote get-url origin)
HOST=$(echo "$ORIGIN" | sed -E 's#^(ssh|https?)://##; s#^[^@]*@##; s#[:/].*##')
NS=$(echo "$ORIGIN" | sed -E 's#\.git$##; s#/[^/]*$##; s#.*[:/]##')
API=https://$HOST/api/v4
ASSETS=${ASSETS_PROJECT:-$NS/skullburglar-assets}
PROJ=$(echo "$ASSETS" | sed 's#/#%2F#g')
auth() { curl -fsS -H "PRIVATE-TOKEN: $GITLAB_TOKEN" "$@"; }

W=$REPO/out/codegen-publish
rm -rf "$W"
mkdir -p "$W/tree/game"
git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - | tar -xf - -C "$W/tree"
cp -L "$REPO/game/default.xex" "$W/tree/game/"  # a copy: codegen refuses an executable that resolves outside game/
tools/ci/sdk.sh "$W/sdk"

KEY=$(tools/ci/codegen_key.sh "$W/sdk")
if auth "$API/projects/$PROJ/packages?package_type=generic&package_name=codegen&package_version=$KEY" | grep -q '"id"'
then
  echo "codegen output $KEY is already stored"
  exit 0
fi

CODEGEN_ONLY=1 "$W/tree/tools/ci/build.sh" "$PLATFORM" "$W/sdk" "$W/build"
# codegen.d lists this machine's absolute paths; the build writes its own.
tar --exclude default/codegen.d -cJf "$W/codegen.tar.xz" -C "$W/tree/generated" default
ls -la "$W/codegen.tar.xz"
auth --upload-file "$W/codegen.tar.xz" "$API/projects/$PROJ/packages/generic/codegen/$KEY/codegen.tar.xz" >/dev/null
echo "stored codegen output $KEY in $ASSETS"
