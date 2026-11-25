#!/bin/sh
# B-408: the update code's scratch files follow portable mode. Builds tools/update_probe.cpp (the real update_install.cpp
# in a stand-in program) and runs its start-up clean-up twice with a stale update folder in the system temp folder and
# another in the portable data folder's tmp/: a portable run (BOTS_DATA_DIR) must find and remove only the second, a
# normal run only the first. Also checks the portable run leaves HOME alone. Needs c++ and a POSIX shell; runs anywhere.
set -eu
R=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
EXTRA=""; [ "$(uname -s)" = Darwin ] && EXTRA="-framework CoreFoundation"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -pthread $EXTRA -I "$R/src" -DBOTS_VERSION='"0.0.1"' -DBOTS_UPDATE_URL='""' -DBOTS_UPDATE_PUBKEY='""' \
  "$R/tools/update_probe.cpp" "$R/src/setup/update_check.cpp" "$R/src/setup/update_logic.cpp" \
  "$R/src/setup/ed25519.cpp" "$R/src/setup/update_install.cpp" -o "$W/probe"
fail() { echo "FAIL: $*" >&2; exit 1; }
mkdir -p "$W/home" "$W/systmp" "$W/data/tmp"
stale() { mkdir -p "$1/skullburglar-update-$2"; echo x > "$1/skullburglar-update-$2/file"; touch -t 202001010000 "$1/skullburglar-update-$2/file" "$1/skullburglar-update-$2"; }
stale "$W/systmp" system
stale "$W/data/tmp" portable
HOME=$W/home TMPDIR=$W/systmp BOTS_DATA_DIR=$W/data "$W/probe" clean >/dev/null
[ ! -e "$W/data/tmp/skullburglar-update-portable" ] || fail "portable run left its own stale update folder"
[ -e "$W/systmp/skullburglar-update-system" ] || fail "portable run touched the system temp folder"
[ -z "$(ls -A "$W/home")" ] || fail "portable run wrote into HOME: $(ls -A "$W/home")"
stale "$W/data/tmp" portable
HOME=$W/home TMPDIR=$W/systmp "$W/probe" clean >/dev/null
[ ! -e "$W/systmp/skullburglar-update-system" ] || fail "normal run left a stale update folder in the system temp folder"
[ -e "$W/data/tmp/skullburglar-update-portable" ] || fail "normal run touched the portable data folder"
echo "portable_probe_test: ok (portable scratch is <data>/tmp, normal scratch is the system temp)"
