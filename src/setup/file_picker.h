// Native "open file" dialogs for the launcher (B-061): the disc image, and the folder for the game files (desktop
// builds). Blocking and modal; call them on the UI thread. They return an empty path when cancelled or unavailable
// (the setup window also takes a typed disc image path).
#pragma once

#include <filesystem>

namespace bots::setup {

std::filesystem::path PickIsoFile();
std::filesystem::path PickFolder();

}  // namespace bots::setup
