// Native "open file" dialog for picking the disc image (B-061). Blocking and modal; call it on the
// UI thread. Returns an empty path when cancelled or unavailable (the setup window also takes a
// typed path).
#pragma once

#include <filesystem>

namespace bots::setup {

std::filesystem::path PickIsoFile();

}  // namespace bots::setup
