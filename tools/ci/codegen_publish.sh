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
# In CI (the pipeline's `codegen` job) it runs with CI_JOB_TOKEN instead: the game executable comes from the assets
# project, the job token (which must be allowed to write there) authenticates the upload, and it stops in seconds when
# the key is stored. The job's resource_group serializes pipelines, so two with the same missing key don't both upload.
set -eu
cd "$(dirname "$0")/../.."
REPO=$PWD
case $(uname -s) in Darwin) PLATFORM=macos ;; Linux) PLATFORM=linux ;; *) echo "macOS or Linux only" >&2; exit 1 ;; esac

if [ -n "${CI_JOB_TOKEN:-}" ]; then
  : "${CI_SERVER_HOST:?}" "${CI_API_V4_URL:?}" "${ASSETS_PROJECT:?}"
  API=$CI_API_V4_URL
  ASSETS=$ASSETS_PROJECT
  AUTH="JOB-TOKEN: $CI_JOB_TOKEN"
else
  : "${GITLAB_TOKEN:?set GITLAB_TOKEN (api scope)}"
  # The GitLab instance and namespace this checkout comes from.
  ORIGIN=$(git remote get-url origin)
  HOST=$(echo "$ORIGIN" | sed -E 's#^(ssh|https?)://##; s#^[^@]*@##; s#[:/].*##')
  NS=$(echo "$ORIGIN" | sed -E 's#\.git$##; s#/[^/]*$##; s#.*[:/]##')
  API=https://$HOST/api/v4
  ASSETS=${ASSETS_PROJECT:-$NS/skullburglar-assets}
  AUTH="PRIVATE-TOKEN: $GITLAB_TOKEN"
fi
PROJ=$(echo "$ASSETS" | sed 's#/#%2F#g')
auth() { curl -fsS --retry 3 -H "$AUTH" "$@"; }
# CODEGEN_PACKAGE is for testing the job against a throwaway package; releases always use "codegen".
PKG=${CODEGEN_PACKAGE:-codegen}

W=$REPO/out/codegen-publish
rm -rf "$W"
mkdir -p "$W/tree/game"
tools/ci/sdk.sh "$W/sdk"
if [ -n "${CI_JOB_TOKEN:-}" ]; then
  rm -rf "$W/assets"
  git clone -q --depth 1 "https://gitlab-ci-token:$CI_JOB_TOKEN@$CI_SERVER_HOST/$ASSETS.git" "$W/assets"
  cp "$W/assets/default.xex" "$W/tree/game/"
fi

# The key reads default.xex.sha1 (in the tree), not the executable, so it's known before anything is copied.
KEY=$(tools/ci/codegen_key.sh "$W/sdk")
# Probe by downloading the first byte (works with a job token, which can't list the assets project's packages).
if auth -o /dev/null -r 0-0 "$API/projects/$PROJ/packages/generic/$PKG/$KEY/codegen.tar.xz" 2>/dev/null
then
  echo "codegen output $KEY is already stored"
  exit 0
fi

git -c safe.directory="*" ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - | tar -xf - -C "$W/tree"
[ -f "$W/tree/game/default.xex" ] || cp -L "$REPO/game/default.xex" "$W/tree/game/"  # a copy: codegen refuses an executable that resolves outside game/

CODEGEN_ONLY=1 "$W/tree/tools/ci/build.sh" "$PLATFORM" "$W/sdk" "$W/build"
# codegen.d lists this machine's absolute paths; the build writes its own.
tar --exclude default/codegen.d -cJf "$W/codegen.tar.xz" -C "$W/tree/generated" default
ls -la "$W/codegen.tar.xz"
[ -f "$W/tree/generated/default/sources.cmake" ] || { echo "codegen wrote no sources.cmake" >&2; exit 1; }
auth --upload-file "$W/codegen.tar.xz" "$API/projects/$PROJ/packages/generic/$PKG/$KEY/codegen.tar.xz" >/dev/null
# Read it back: what is stored must be what was built, or the builds restore a damaged tree.
auth -o "$W/check.tar.xz" "$API/projects/$PROJ/packages/generic/$PKG/$KEY/codegen.tar.xz"
cmp "$W/codegen.tar.xz" "$W/check.tar.xz" || { echo "stored codegen output $KEY differs from the upload" >&2; exit 1; }
echo "stored codegen output $KEY in $ASSETS"
