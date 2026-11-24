#!/bin/sh
# Builds tools/launcher_preview.cpp (the launcher's menu without the game) and runs it:
#   tools/launcher_preview.sh <imgui dir> [launcher_preview arguments]
# <imgui dir>: Dear ImGui's sources at the SDK's version (the SDK checkout's thirdparty/imgui, or a clone of
# github.com/ocornut/imgui at v1.92.5). PREVIEW_BIN=<path> keeps the binary there.
set -eu
IMGUI=${1:?usage: tools/launcher_preview.sh <imgui dir> [arguments]}
shift
REPO=$(cd "$(dirname "$0")/.." && pwd)
BIN=${PREVIEW_BIN:-$(mktemp -d)/launcher_preview}
${CXX:-c++} -std=c++20 -O2 -Wall -Werror -I "$REPO/src" -I "$IMGUI" \
  "$REPO/tools/launcher_preview.cpp" "$REPO/src/setup/launcher_menu.cpp" "$REPO/src/setup/launcher_shot.cpp" \
  "$REPO/src/setup/settings_catalog.cpp" "$REPO/src/setup/loading_screen.cpp" \
  "$IMGUI/imgui.cpp" "$IMGUI/imgui_draw.cpp" "$IMGUI/imgui_widgets.cpp" "$IMGUI/imgui_tables.cpp" \
  -o "$BIN"
exec "$BIN" "$@"
