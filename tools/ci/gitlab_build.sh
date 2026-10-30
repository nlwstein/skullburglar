#!/bin/sh
# One platform's release build in GitLab CI, inside its build image or on the macOS runner (.gitlab-ci.yml):
#   tools/ci/gitlab_build.sh <linux|windows|macos|ios|tvos|android>
# Gets the game executable and shader refs from the private assets project, restores the codegen output the pipeline's
# `codegen` job stored for this tree (tools/ci/codegen_publish.sh; a desktop build without it runs codegen itself), builds and packages, and uploads the packages to this project's
# package registry as <PACKAGE>/<VERSION> (PACKAGE defaults to skullburglar). iOS and tvOS (experimental) build an
# unsigned IPA instead (tools/ios/build.sh; signed when it's installed): skullburglar-<VERSION>-<ios|tvos>.ipa. Android
# (experimental) builds an arm64 APK (tools/android/build.sh): skullburglar-<VERSION>-android.apk, signed with the
# ANDROID_KEYSTORE file variable (the keystore, base64; password ANDROID_KEYSTORE_PASS) when the project has one, else
# a throwaway debug key.
# Env: the job's CI_* variables, VERSION, ASSETS_PROJECT (the assets project path); PACKAGE, UPDATE_URL (optional, see
# build.sh).
set -eu
PLATFORM=${1:?usage: tools/ci/gitlab_build.sh <linux|windows|macos|ios|tvos|android>}
cd "$(dirname "$0")/../.."
PACKAGE=${PACKAGE:-skullburglar}
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
if get -o out/codegen.tar.xz "$CI_API_V4_URL/projects/$PROJ/packages/generic/${CODEGEN_PACKAGE:-codegen}/$KEY/codegen.tar.xz" 2>/dev/null
then
  tar -xJf out/codegen.tar.xz -C generated
  echo "codegen: using stored output $KEY"
else
  echo "codegen: nothing stored for $KEY, so codegen runs here (the pipeline's codegen job should have stored it)"
fi

case $PLATFORM in android)
  # Cross-compiled with the NDK, which can't run the recompiler either: stored codegen output only, as for iOS.
  [ -f generated/default/sources.cmake ] || { echo "no stored codegen output for $KEY: the pipeline's codegen job should have stored it (tools/ci/codegen_publish.sh stores it by hand)" >&2; exit 1; }
  # Releases keep one signing key, so an installed copy updates in place (an uninstall deletes the game files).
  if [ -n "${ANDROID_KEYSTORE:-}" ]; then
    mkdir -p out/android
    base64 -d "$ANDROID_KEYSTORE" > out/android/release.keystore
    export KEYSTORE=out/android/release.keystore KEYSTORE_PASS=${ANDROID_KEYSTORE_PASS:?}
  elif [ "$PACKAGE" = skullburglar ]; then
    echo "no ANDROID_KEYSTORE: a release APK needs the release signing key" >&2
    exit 1
  fi
  step build
  # PACKAGE here is the registry package; tools/android/build.sh reads PACKAGE as the application id.
  (unset PACKAGE; SDK=out/sdk VERSION=$VERSION tools/android/build.sh build apk)
  rm -f out/android/release.keystore
  step upload
  name=skullburglar-$VERSION-android.apk
  get --upload-file out/android/Skullburglar.apk \
    "$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/$PACKAGE/$VERSION/$name" >/dev/null
  echo "uploaded $name as $PACKAGE/$VERSION"
  exit 0
esac

case $PLATFORM in ios|tvos)
  # The recompiler doesn't run on iOS, so these builds need stored codegen output for this tree.
  [ -f generated/default/sources.cmake ] || { echo "no stored codegen output for $KEY: the pipeline's codegen job should have stored it (tools/ci/codegen_publish.sh stores it by hand)" >&2; exit 1; }
  step build
  # iOS and tvOS are MoltenVK's device class 3, like macOS.
  PLATFORM=$PLATFORM SDK=out/sdk REFS=out/assets/pipelines/class3.refs JOBS=$(sysctl -n hw.ncpu) VERSION=$VERSION \
    tools/ios/build.sh build bundle ipa
  step upload
  name=skullburglar-$VERSION-$PLATFORM.ipa
  get --upload-file "out/$PLATFORM/Skullburglar-$PLATFORM.ipa" \
    "$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/$PACKAGE/$VERSION/$name" >/dev/null
  echo "uploaded $name as $PACKAGE/$VERSION"
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
  get --upload-file "$f" "$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/$PACKAGE/$VERSION/${f##*/}" >/dev/null
  echo "uploaded ${f##*/}"
done
