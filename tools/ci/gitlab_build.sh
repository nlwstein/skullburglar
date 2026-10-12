#!/bin/sh
# One platform's release build in GitLab CI, inside its build image or on the macOS runner (.gitlab-ci.yml):
#   tools/ci/gitlab_build.sh <linux|windows|macos|ios|tvos>
# Gets the game executable and shader refs from the private assets project, restores stored codegen output when there
# is some for this tree (tools/ci/codegen_publish.sh), builds and packages, and uploads the packages to this project's
# package registry as skullburglar/<VERSION>. iOS and tvOS (experimental) build an unsigned IPA instead
# (tools/ios/build.sh; signed when it's installed), uploaded as skullburglar-apple/<VERSION>.
# Env: the job's CI_* variables, VERSION, ASSETS_PROJECT (the assets project path); UPDATE_URL (optional, see build.sh).
set -eu
PLATFORM=${1:?usage: tools/ci/gitlab_build.sh <linux|windows|macos|ios|tvos>}
cd "$(dirname "$0")/../.."
: "${CI_JOB_TOKEN:?}" "${CI_SERVER_HOST:?}" "${CI_API_V4_URL:?}" "${CI_PROJECT_ID:?}" "${VERSION:?}" "${ASSETS_PROJECT:?}"
step() { echo "== $* ($(date -u +%H:%M:%S))"; }
# The job token only reads the assets project because its allowlist names this project.
get() { curl -fsS --retry 3 -H "JOB-TOKEN: $CI_JOB_TOKEN" "$@"; }

step assets
rm -rf out/assets
git clone -q --depth 1 "https://gitlab-ci-token:$CI_JOB_TOKEN@$CI_SERVER_HOST/$ASSETS_PROJECT.git" out/assets
mkdir -p game
cp out/assets/default.xex game/

step sdk
rm -rf out/sdk
tools/ci/sdk.sh out/sdk

KEY=$(tools/ci/codegen_key.sh out/sdk)
PROJ=$(echo "$ASSETS_PROJECT" | sed 's#/#%2F#g')
rm -rf generated/default
if get -o out/codegen.tar.xz "$CI_API_V4_URL/projects/$PROJ/packages/generic/codegen/$KEY/codegen.tar.xz" 2>/dev/null
then
  tar -xJf out/codegen.tar.xz -C generated
  echo "codegen: using stored output $KEY"
else
  echo "codegen: nothing stored for $KEY, so codegen runs here (store it with tools/ci/codegen_publish.sh)"
fi

case $PLATFORM in ios|tvos)
  # The recompiler doesn't run on iOS, so these builds need stored codegen output for this tree.
  [ -f generated/default/sources.cmake ] || { echo "no stored codegen output for $KEY: run tools/ci/codegen_publish.sh" >&2; exit 1; }
  step build
  # iOS and tvOS are MoltenVK's device class 3, like macOS.
  PLATFORM=$PLATFORM SDK=out/sdk REFS=out/assets/pipelines/class3.refs JOBS=$(sysctl -n hw.ncpu) \
    tools/ios/build.sh build bundle ipa
  step upload
  f=out/$PLATFORM/Skullburglar-$PLATFORM.ipa
  get --upload-file "$f" "$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/skullburglar-apple/$VERSION/${f##*/}" >/dev/null
  echo "uploaded ${f##*/} as skullburglar-apple/$VERSION"
  exit 0
esac

# Shader refs are per device class: class 3 is MoltenVK; Windows and Linux GPUs (the Deck included) are class 7. Refs
# for another class are ignored at run time, which leaves the first boot with no shader warm-up.
case $PLATFORM in macos) CLASS=3 ;; *) CLASS=7 ;; esac
REFS=out/assets/pipelines/class$CLASS.refs tools/ci/build.sh "$PLATFORM" out/sdk out/ci
step package
tools/ci/package.sh "$PLATFORM" out/ci

step upload
for f in out/ci/release/*; do
  get --upload-file "$f" "$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/skullburglar/$VERSION/${f##*/}" >/dev/null
  echo "uploaded ${f##*/}"
done
