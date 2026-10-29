// Persistent per-user locations for the first-run setup (B-061). Game files extracted from the
// player's disc live outside the install, so they survive relaunches, updates and reinstalls:
//   Windows  %LOCALAPPDATA%\bots-recomp\game
//   macOS    ~/Library/Application Support/bots-recomp/game
//   Linux    $XDG_DATA_HOME/bots-recomp/game (default ~/.local/share/bots-recomp/game)
// On the desktop builds the player may keep them elsewhere instead: a folder chosen in the launcher (bots.toml's
// game_data_root), or a portable install's game folder next to the executable (PortableGameDir).
// Settings (bots.toml) live in the user data root next to the saves, so test runs with their own
// --user_data_root never read or write the player's settings.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "../path_utf8.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace bots::setup {

inline std::filesystem::path AppDataDir() {
  namespace fs = std::filesystem;
#if defined(_WIN32)
  if (const fs::path local = bots::EnvPath("LOCALAPPDATA"); !local.empty()) return local / "bots-recomp";
#elif defined(__APPLE__)
  if (const fs::path home = bots::EnvPath("HOME"); !home.empty()) {
    return home / "Library" / "Application Support" / "bots-recomp";
  }
#else
  if (const fs::path xdg = bots::EnvPath("XDG_DATA_HOME"); !xdg.empty()) return xdg / "bots-recomp";
  if (const fs::path home = bots::EnvPath("HOME"); !home.empty()) return home / ".local" / "share" / "bots-recomp";
#endif
  return fs::current_path() / "bots-recomp";
}

inline std::filesystem::path DefaultGameDir() { return AppDataDir() / "game"; }

// Running from a macOS app bundle (Skullburglar.app/Contents/MacOS/bots).
inline bool InAppBundle(const std::filesystem::path& exe_dir) {
#if defined(__APPLE__)
  return exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents";
#else
  (void)exe_dir;
  return false;
#endif
}

// In an app bundle, which is read-only to the game, logs go to ~/Library/Logs/Skullburglar instead of logs/ next to
// the executable. Empty when not in a bundle.
inline std::filesystem::path BundleLogDir(const std::filesystem::path& exe_dir) {
#if defined(__APPLE__)
  if (InAppBundle(exe_dir)) {
    if (const std::filesystem::path home = bots::EnvPath("HOME"); !home.empty()) {
      return home / "Library" / "Logs" / "Skullburglar";
    }
  }
#endif
  (void)exe_dir;
  return {};
}

// A usable game data root has the entry XEX and the game's package folder.
inline bool IsGameDir(const std::filesystem::path& dir) {
  std::error_code ec;
  return !dir.empty() && std::filesystem::is_regular_file(dir / "default.xex", ec) &&
         std::filesystem::is_directory(dir / "C1Game", ec);
}

// A portable install keeps the game files in a folder named game next to the executable. Desktop builds only, and
// not in a macOS app bundle; empty where there is none. iOS and tvOS keep everything in the app container.
inline std::filesystem::path PortableGameDir(const std::filesystem::path& exe_dir) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  (void)exe_dir;
  return {};
#else
  if (exe_dir.empty() || InAppBundle(exe_dir)) return {};
  return exe_dir / "game";
#endif
}

// The game files when neither the command line nor bots.toml names them: a portable install's game folder when it
// holds the game, else the per-user default. Without a portable game folder this is DefaultGameDir(), as before.
inline std::filesystem::path AutoGameDir(const std::filesystem::path& exe_dir) {
  const std::filesystem::path portable = PortableGameDir(exe_dir);
  return IsGameDir(portable) ? portable : DefaultGameDir();
}

// The game folder for a folder the player chose in the launcher: the folder itself when it already holds the game's
// files, else its game subfolder. The game's files are never spread into a folder that holds something else, so the
// chosen folder is the target only when it already holds the game, and a game subfolder that holds something else is
// refused: the result is empty and `error` says why. A reinstall replaces only the game's own entries (game_swap.h).
inline std::filesystem::path ChosenGameDir(const std::filesystem::path& chosen, std::string& error) {
  namespace fs = std::filesystem;
  error.clear();
  if (chosen.empty()) return {};
  if (IsGameDir(chosen)) return chosen;
  const fs::path target = chosen / "game";
  if (IsGameDir(target)) return target;
  std::error_code ec;
  if (fs::exists(target, ec) && !(fs::is_directory(target, ec) && fs::is_empty(target, ec))) {
    error = bots::PathUtf8(target) + " already exists and doesn't hold the game's files. Choose another folder.";
    return {};
  }
  return target;
}

}  // namespace bots::setup
