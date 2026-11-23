// Where the game files are, and the offer to copy them into a portable folder (B-400).
//
// Portable mode (portable.h) keeps saves, settings, the shader cache and logs in one folder. The game files belong in
// it too, so in portable mode the default game folder is <data root>/game, next to user/, cache/ and logs/, on every
// desktop platform. Outside portable mode nothing changes: a game folder next to the executable when it holds the
// game, else the per-user default (app_paths.h: the "shared" location, because every normal install on the machine
// uses it). A folder the player chose in the launcher (bots.toml's game_data_root) always wins, and so does
// --game_data_root.
//
// A portable start whose own game folder is empty but whose machine has a shared install gets an offer to copy it
// (never move: the shared copy stays for the normal install): copy it here, install from a disc image instead, or not
// now. "Not now" is remembered in the data root (a small marker file) and the offer isn't made again; the Game files
// page keeps a "Copy from ..." button for the player to ask. Players who played portable before 1.13.1 used the shared
// folder without being told: they get the same offer, and "Not now" leaves them with the normal first-run state plus a
// "Use it where it is" button (saved as a chosen folder), so nothing switches silently in either direction.
//
// The decisions are pure (paths as text plus an "is this a game folder" callback), so tests/game_location_test.cpp
// covers every platform's layout on any host. The copy itself uses the file system and is tested on temp folders.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

#include "../path_utf8.h"
#include "game_swap.h"
#include "iso_extract.h"

namespace bots::setup {

using IsGameDirFn = std::function<bool(const std::filesystem::path&)>;

enum class GameDirWhy {
  kCommandLine,     // --game_data_root
  kChosen,          // game_data_root in bots.toml: the player chose it in the launcher
  kPortableDefault, // <data root>/game
  kNextToProgram,   // a game folder next to the executable that holds the game (before 1.13.1 portable installs)
  kShared,          // the per-user default
};

inline const char* WhyText(GameDirWhy why) {
  switch (why) {
    case GameDirWhy::kCommandLine: return "command line";
    case GameDirWhy::kChosen: return "chosen";
    case GameDirWhy::kPortableDefault: return "portable default";
    case GameDirWhy::kNextToProgram: return "next to the program";
    case GameDirWhy::kShared: return "shared";
  }
  return "";
}

struct GameLocationInput {
  std::filesystem::path command_line;     // --game_data_root (empty: not given)
  std::filesystem::path configured;       // bots.toml's game_data_root (empty: none)
  bool portable = false;                  // portable.txt or BOTS_DATA_DIR
  std::filesystem::path data_root;        // the portable data folder (portable only)
  std::filesystem::path next_to_program;  // PortableGameDir(exe folder), empty where there is none
  std::filesystem::path shared;           // DefaultGameDir(): the per-user default
};

struct GameLocation {
  std::filesystem::path dir;          // the game files are read from here
  GameDirWhy why = GameDirWhy::kShared;
  std::filesystem::path default_dir;  // a fresh install goes here; "Use the default folder"
  std::filesystem::path shared;       // the per-user default (the copy offer's source)
  bool portable = false;
  std::filesystem::path data_root;
};

// Picks the game folder. Order: command line, a chosen folder, then the defaults. Portable: <data root>/game when it
// holds the game, else a game folder next to the program that does (the 1.12/1.13 layout), else <data root>/game
// anyway (empty: the launcher offers to fill it). Not portable: exactly as before 1.13.1.
inline GameLocation ResolveGameLocation(const GameLocationInput& in, const IsGameDirFn& is_game) {
  GameLocation out;
  out.shared = in.shared;
  out.portable = in.portable;
  out.data_root = in.data_root;
  const std::filesystem::path portable_game = in.data_root / "game";
  out.default_dir = in.portable ? portable_game : in.shared;
  if (!in.command_line.empty()) {
    out.dir = in.command_line;
    out.why = GameDirWhy::kCommandLine;
  } else if (!in.configured.empty()) {
    out.dir = in.configured;
    out.why = GameDirWhy::kChosen;
  } else if (in.portable && is_game(portable_game)) {
    out.dir = portable_game;
    out.why = GameDirWhy::kPortableDefault;
  } else if (!in.next_to_program.empty() && is_game(in.next_to_program)) {
    out.dir = in.next_to_program;
    out.why = GameDirWhy::kNextToProgram;
  } else {
    out.dir = out.default_dir;
    out.why = in.portable ? GameDirWhy::kPortableDefault : GameDirWhy::kShared;
  }
  return out;
}

// What this run resolved to (set once by the app's OnConfigurePaths; the launcher and the log read it).
inline GameLocation& ActiveGameLocation() {
  static GameLocation active;
  return active;
}

// Whether "Copy from <shared>" makes sense for the folder in use now: portable, the folder in use is the portable
// default and holds no game, and the shared location holds one (and is a different folder).
inline bool CanCopyFromShared(bool portable, const std::filesystem::path& game_dir,
                              const std::filesystem::path& default_dir, const std::filesystem::path& shared,
                              const IsGameDirFn& is_game) {
  if (!portable || shared.empty() || default_dir.empty()) return false;
  if (SamePath(game_dir, shared) || !SamePath(game_dir, default_dir) || SamePath(default_dir, shared)) return false;
  return !is_game(game_dir) && is_game(shared);
}

// Whether the launcher offers the copy when it opens: the same conditions, a game folder that was not given on the
// command line, and a player who hasn't already answered "Not now" in this data root.
inline bool ShouldOfferCopy(const GameLocation& loc, bool declined, const IsGameDirFn& is_game) {
  return loc.why == GameDirWhy::kPortableDefault && !declined &&
         CanCopyFromShared(loc.portable, loc.dir, loc.default_dir, loc.shared, is_game);
}

// What "Not now" does: a data folder that has been used before (settings or saves exist: a 1.12/1.13 portable install that
// played from the shared folder) keeps using the shared folder, saved as the chosen folder, so an update never looks like
// the game broke. A brand-new portable folder stays in the normal first-run state.
inline bool NotNowUsesShared(bool data_folder_used_before) { return data_folder_used_before; }

// The marker "Not now" leaves in the data root.
inline std::filesystem::path CopyDeclinedMarker(const std::filesystem::path& data_root) {
  return data_root / "game-copy-declined";
}

// "Not now" is the player's own word; a marker that can't be written only means the offer comes again.
inline void RememberCopyDeclined(const std::filesystem::path& data_root) {
  std::error_code ec;
  std::filesystem::create_directories(data_root, ec);
  if (std::FILE* f = bots::OpenFile(CopyDeclinedMarker(data_root), "wb")) {
    std::fputs("Not now was chosen for the offer to copy the shared game files here.\n", f);
    std::fclose(f);
  }
}

inline bool CopyDeclined(const std::filesystem::path& data_root) {
  std::error_code ec;
  return std::filesystem::is_regular_file(CopyDeclinedMarker(data_root), ec);
}

// Enough room for `needed` bytes plus the same 256 MB margin an install keeps.
inline bool EnoughSpace(uint64_t available, uint64_t needed) { return available >= needed + (256ull << 20); }

inline std::string SpaceMessage(const std::filesystem::path& where, uint64_t needed, uint64_t available) {
  char text[64];
  std::snprintf(text, sizeof(text), "needs %.1f GB, %.1f GB free.", double(needed) / 1e9, double(available) / 1e9);
  return "Not enough free space in " + bots::PlainPathUtf8(where) + ": " + text;
}

struct GameSize {
  uint64_t bytes = 0;
  uint32_t files = 0;
  std::string error;  // set when the folder couldn't be read completely
};

// Totals the regular files below `dir`.
inline GameSize MeasureGameDir(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  GameSize out;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code e;
    if (!it->is_regular_file(e)) continue;
    const auto size = it->file_size(e);
    if (e) {
      out.error = "Can't read " + bots::PlainPathUtf8(it->path()) + " (" + e.message() + ").";
      return out;
    }
    out.bytes += size;
    ++out.files;
  }
  if (ec) out.error = "Can't read " + bots::PlainPathUtf8(dir) + " (" + ec.message() + ").";
  return out;
}

// Called on the finished copy before it is swapped in: the install's own check (default.xex is this game's, by title
// id). Returns an error sentence, or empty when it passes.
using VerifyFn = std::function<std::string(const std::filesystem::path& root)>;

namespace detail {

// Copies one file in 4 MB pieces so progress and Cancel stay live on a multi-gigabyte file.
inline bool CopyFileChunked(const std::filesystem::path& from, const std::filesystem::path& to,
                            ExtractProgress& progress, std::string& error) {
  std::FILE* in = bots::OpenFile(from, "rb");
  if (!in) {
    error = "Can't read " + bots::PlainPathUtf8(from) + ".";
    return false;
  }
  std::FILE* out = bots::OpenFile(to, "wb");
  if (!out) {
    std::fclose(in);
    error = "Can't write " + bots::PlainPathUtf8(to) + ".";
    return false;
  }
  std::string buffer(4 << 20, '\0');
  bool ok = true;
  while (ok) {
    if (progress.cancel.load()) {
      ok = false;
      break;
    }
    const size_t n = std::fread(buffer.data(), 1, buffer.size(), in);
    if (n == 0) {
      if (std::ferror(in)) {
        error = "Read failed for " + bots::PlainPathUtf8(from) + ".";
        ok = false;
      }
      break;
    }
    if (std::fwrite(buffer.data(), 1, n, out) != n) {
      error = "Write failed for " + bots::PlainPathUtf8(to) + " (disk full?)";
      ok = false;
      break;
    }
    progress.bytes_done += n;
  }
  std::fclose(in);
  if (std::fclose(out) != 0 && ok) {
    error = "Write failed for " + bots::PlainPathUtf8(to) + " (disk full?)";
    ok = false;
  }
  return ok;
}

}  // namespace detail

// Same tree, same sizes: every file of `src` is in `dest` with the same length, and nothing else is.
inline std::string VerifyCopyMatches(const std::filesystem::path& src, const std::filesystem::path& dest,
                                     const GameSize& expected) {
  namespace fs = std::filesystem;
  const GameSize got = MeasureGameDir(dest);
  if (!got.error.empty()) return got.error;
  if (got.files != expected.files || got.bytes != expected.bytes) {
    char text[96];
    std::snprintf(text, sizeof(text), "%u files and %.3f GB were copied, %u files and %.3f GB expected.", got.files,
                  double(got.bytes) / 1e9, expected.files, double(expected.bytes) / 1e9);
    return std::string("The copy doesn't match the original: ") + text;
  }
  std::error_code ec;
  for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code e;
    if (!it->is_regular_file(e)) continue;
    const fs::path rel = it->path().lexically_relative(src);
    const auto want = it->file_size(e);
    std::error_code e2;
    const auto have = fs::file_size(dest / rel, e2);
    if (e || e2 || have != want) return "The copy of " + bots::PlainPathUtf8(rel) + " doesn't match the original.";
  }
  return {};
}

// Copies the game folder `src` into `dest` (a new folder, or an empty one): into <dest>.partial first, checked the way an
// install is (the same files and sizes; the game's folder shape; `verify`), then swapped in, so an interrupted or failed
// copy never leaves anything that looks installed. On cancel or failure the partial copy is removed. The shared copy is
// only read. Blocking; run it on a worker thread; cancel through progress.cancel.
inline ExtractResult CopyGameFiles(const std::filesystem::path& src, const std::filesystem::path& dest,
                                   ExtractProgress& progress, const IsGameDirFn& is_game, const VerifyFn& verify,
                                   const std::function<void()>& after_file = {}) {
  namespace fs = std::filesystem;
  ExtractResult result;
  std::error_code ec;
  const GameSize size = MeasureGameDir(src);
  if (!size.error.empty() || size.files == 0) {
    result.error = size.error.empty() ? "There are no game files in " + bots::PlainPathUtf8(src) + "." : size.error;
    return result;
  }
  progress.bytes_total = size.bytes;
  progress.files_total = size.files;
  const fs::path parent = dest.parent_path();
  fs::create_directories(parent, ec);
  const auto space = fs::space(parent, ec);
  if (!ec && !EnoughSpace(space.available, size.bytes)) {
    result.error = SpaceMessage(parent, size.bytes, space.available);
    return result;
  }
  fs::path partial = dest;
  partial += ".partial";
  fs::remove_all(partial, ec);
  fs::create_directories(partial, ec);
  if (ec) {
    result.error = "Can't create " + bots::PlainPathUtf8(partial) + ": " + ec.message();
    return result;
  }
  auto fail = [&](std::string why) {
    fs::remove_all(partial, ec);
    result.cancelled = progress.cancel.load();  // a plain "Cancelled." note in the launcher, as for an install
    result.error = result.cancelled ? "Cancelled." : std::move(why);
    return result;
  };
  for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (progress.cancel.load()) return fail({});
    std::error_code e;
    const fs::path rel = it->path().lexically_relative(src);
    if (it->is_directory(e)) {
      fs::create_directories(partial / rel, e);
      if (e) return fail("Can't create " + bots::PlainPathUtf8(partial / rel) + ": " + e.message());
      continue;
    }
    if (!it->is_regular_file(e)) continue;
    progress.SetCurrent(bots::PathUtf8(rel));
    std::string error;
    if (!detail::CopyFileChunked(it->path(), partial / rel, progress, error)) return fail(error);
    ++progress.files_done;
    if (after_file) after_file();  // tests press Cancel here
  }
  if (ec) return fail("Can't read " + bots::PlainPathUtf8(src) + " (" + ec.message() + ").");
  if (progress.cancel.load()) return fail({});
  if (std::string why = VerifyCopyMatches(src, partial, size); !why.empty()) return fail(std::move(why));
  if (!is_game(partial)) return fail("The copy doesn't hold the game's files (default.xex or C1Game is missing).");
  if (verify) {
    if (std::string why = verify(partial); !why.empty()) return fail(std::move(why));
  }
  if (!SwapInGameFiles(partial, dest, result.error)) {
    fs::remove_all(partial, ec);
    return result;
  }
  result.ok = true;
  return result;
}

}  // namespace bots::setup
