#!/bin/sh
# Makes a CI build image available as a local tag without rebuilding it when it can help it (.gitlab-ci.yml):
#   tools/ci/ci_image.sh <docker dir> <local tag>
# The image is keyed by the content of its docker/<dir>: an unchanged Dockerfile reuses the image already on the runner's
# Docker host, else the copy stored in this project's (private) package registry as ci-image/<key>/<dir>.tar.gz, and only
# a changed Dockerfile builds a new one, which is then stored there too. A rebuild of an unchanged Dockerfile isn't the
# same image: the Windows image's xwin downloads whatever CRT and SDK Microsoft publishes that day, and 0.6.6 stopped
# reading the 10.0.26100 SDK's new packaging (2026-11), so a pruned build cache broke the Windows build. The stored copy
# holds Microsoft's CRT and SDK: it stays in this private project for its own CI, never anywhere public.
# Runs in docker:29-cli (Alpine). Env: the job's CI_JOB_TOKEN, CI_API_V4_URL, CI_PROJECT_ID.
set -eu
DIR=${1:?usage: tools/ci/ci_image.sh <docker dir> <local tag>}
TAG=${2:?usage: tools/ci/ci_image.sh <docker dir> <local tag>}
cd "$(dirname "$0")/../.."
: "${CI_JOB_TOKEN:?}" "${CI_API_V4_URL:?}" "${CI_PROJECT_ID:?}"

# Every file under docker/<dir>, by path and content, in a fixed order.
KEY=$(cd "docker/$DIR" && find . -type f | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -c1-16)
REF=$TAG:$KEY
URL=$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/ci-image/$KEY/$DIR.tar.gz
command -v curl >/dev/null || apk add -q --no-cache curl

if docker image inspect "$REF" >/dev/null 2>&1; then
  echo "ci image: $REF is on this host"
elif curl -fsS --retry 3 -H "JOB-TOKEN: $CI_JOB_TOKEN" "$URL" | gunzip | docker load -q; then
  docker image inspect "$REF" >/dev/null 2>&1 || { echo "ci image: the stored copy for $KEY doesn't hold $REF" >&2; exit 1; }
  echo "ci image: $REF restored from the package registry"
else
  echo "ci image: nothing stored for $KEY (docker/$DIR changed), building it"
  docker build -q -t "$REF" "docker/$DIR"
  # Through a file: the instance refuses a streamed (chunked) upload of this size.
  docker save "$REF" | gzip -1 > "/tmp/ci-image-$KEY.tar.gz"
  curl -fsS --retry 3 -H "JOB-TOKEN: $CI_JOB_TOKEN" --upload-file "/tmp/ci-image-$KEY.tar.gz" "$URL" >/dev/null
  rm -f "/tmp/ci-image-$KEY.tar.gz"
  echo "ci image: $REF stored as ci-image/$KEY/$DIR.tar.gz"
fi
docker tag "$REF" "$TAG"
