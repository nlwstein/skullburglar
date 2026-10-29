// Moves a finished extraction into the game folder (ExtractIso). Kept free of SDK headers so it can be tested alone.
#pragma once

#include "../path_utf8.h"
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace bots::setup {

// Moves every top-level entry of `partial` into `dest`, replacing only the entries of the same name: anything else
// already in `dest` (the player's disc image, other files) stays. Replaced entries wait in <dest>.old until all moves
// are done, and if one fails everything moves back, so a failed swap leaves the old install as it was. `partial` is
// removed on success. On failure `error` is user-facing.
inline bool SwapInGameFiles(const std::filesystem::path& partial, const std::filesystem::path& dest,
                            std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(dest, ec)) {
    fs::rename(partial, dest, ec);
    if (ec) error = "Can't finish installing to " + bots::PathUtf8(dest) + ": " + ec.message();
    return !ec;
  }
  fs::path old = dest;
  old += ".old";
  fs::remove_all(old, ec);
  fs::create_directory(old, ec);
  if (ec) {
    error = "Can't create " + bots::PathUtf8(old) + ": " + ec.message();
    return false;
  }
  std::vector<fs::path> names;
  for (const auto& entry : fs::directory_iterator(partial, ec)) names.push_back(entry.path().filename());
  if (ec) {
    error = "Can't read " + bots::PathUtf8(partial) + ": " + ec.message();
    fs::remove_all(old, ec);
    return false;
  }
  std::vector<fs::path> replaced, placed;
  for (const auto& name : names) {
    std::error_code missing;  // set when the old install lacks this entry, which is fine
    if (fs::exists(fs::symlink_status(dest / name, missing))) {
      fs::rename(dest / name, old / name, ec);
      if (!ec) replaced.push_back(name);
    }
    if (!ec) fs::rename(partial / name, dest / name, ec);
    if (!ec) {
      placed.push_back(name);
      continue;
    }
    error = "Can't replace the old " + bots::PathUtf8(dest / name) + ": " + ec.message();
    // Put the old install back as it was.
    std::error_code ignored;
    for (const auto& n : placed) fs::rename(dest / n, partial / n, ignored);
    for (const auto& n : replaced) fs::rename(old / n, dest / n, ignored);
    fs::remove_all(old, ignored);
    return false;
  }
  fs::remove_all(old, ec);
  fs::remove_all(partial, ec);
  return true;
}

}  // namespace bots::setup
