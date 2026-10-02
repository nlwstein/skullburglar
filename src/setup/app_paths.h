// Persistent per-user locations for the first-run setup (B-061). Game files extracted from the
// player's disc live outside the install, so they survive relaunches, updates and reinstalls:
//   Windows  %LOCALAPPDATA%\bots-recomp\game
//   macOS    ~/Library/Application Support/bots-recomp/game
//   Linux    $XDG_DATA_HOME/bots-recomp/game (default ~/.local/share/bots-recomp/game)
// Settings (bots.toml) live in the user data root next to the saves, so test runs with their own
// --user_data_root never read or write the player's settings.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace bots::setup {

inline std::filesystem::path AppDataDir() {
  namespace fs = std::filesystem;
#if defined(_WIN32)
  if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
    return fs::path(local) / "bots-recomp";
  }
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home && *home) {
    return fs::path(home) / "Library" / "Application Support" / "bots-recomp";
  }
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    return fs::path(xdg) / "bots-recomp";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return fs::path(home) / ".local" / "share" / "bots-recomp";
  }
#endif
  return fs::current_path() / "bots-recomp";
}

inline std::filesystem::path DefaultGameDir() { return AppDataDir() / "game"; }

// A usable game data root has the entry XEX and the game's package folder.
inline bool IsGameDir(const std::filesystem::path& dir) {
  std::error_code ec;
  return !dir.empty() && std::filesystem::is_regular_file(dir / "default.xex", ec) &&
         std::filesystem::is_directory(dir / "C1Game", ec);
}

}  // namespace bots::setup
