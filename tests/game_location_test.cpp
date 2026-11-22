// Plain-assert tests for src/setup/game_location.h (B-400): which game folder a run uses (every platform's layout, as
// path text), the portable copy offer's decision, and the copy itself on temp folders.
// Run: c++ -std=c++20 -I src tests/game_location_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "setup/game_location.h"

namespace fs = std::filesystem;
namespace su = bots::setup;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// A pretend disk: the folders that "hold the game".
static su::IsGameDirFn Games(const std::set<std::string>& dirs) {
  return [dirs](const fs::path& p) { return dirs.count(p.generic_string()) > 0; };
}

struct Layout {
  const char* name;
  const char* data_root;     // portable data folder
  const char* exe_dir_game;  // a game folder next to the program ("" where there is none)
  const char* shared;        // the per-user default
};
// The data folder sits next to portable.txt (portable.h); the shared folder is app_paths.h's AppDataDir()/game.
static const Layout kLayouts[] = {
    {"windows", "D:/Games/Skullburglar/bots-data", "D:/Games/Skullburglar/game",
     "C:/Users/_p_/AppData/Local/bots-recomp/game"},
    {"macos app", "/Users/_p_/Games/bots-data", "", "/Users/_p_/Library/Application Support/bots-recomp/game"},
    {"linux tarball", "/home/_p_/sb/bots-data", "/home/_p_/sb/game", "/home/_p_/.local/share/bots-recomp/game"},
    {"linux appimage", "/home/_p_/Games/bots-data", "", "/home/_p_/.local/share/bots-recomp/game"},
};

static su::GameLocationInput In(const Layout& l, bool portable) {
  su::GameLocationInput in;
  in.portable = portable;
  if (portable) in.data_root = l.data_root;
  in.next_to_program = l.exe_dir_game;
  in.shared = l.shared;
  return in;
}

int main() {
  for (const Layout& l : kLayouts) {
    const fs::path data_game = fs::path(l.data_root) / "game";
    {  // portable, nothing installed anywhere: the portable default, empty
      auto loc = su::ResolveGameLocation(In(l, true), Games({}));
      CHECK(loc.dir == data_game && loc.why == su::GameDirWhy::kPortableDefault);
      CHECK(loc.default_dir == data_game && loc.shared == fs::path(l.shared));
    }
    {  // portable, a shared install only: still the (empty) portable default, never the shared folder silently
      auto loc = su::ResolveGameLocation(In(l, true), Games({l.shared}));
      CHECK(loc.dir == data_game && loc.why == su::GameDirWhy::kPortableDefault);
    }
    {  // portable with its own game: that one
      auto loc = su::ResolveGameLocation(In(l, true), Games({data_game.generic_string(), l.shared}));
      CHECK(loc.dir == data_game && loc.why == su::GameDirWhy::kPortableDefault);
    }
    for (bool portable : {true, false}) {  // a folder chosen in the launcher wins, then the command line
      auto in = In(l, portable);
      in.configured = "/elsewhere/game";
      auto loc = su::ResolveGameLocation(in, Games({data_game.generic_string(), l.shared}));
      CHECK(loc.dir == fs::path("/elsewhere/game") && loc.why == su::GameDirWhy::kChosen);
      in.command_line = "/cli/game";
      loc = su::ResolveGameLocation(in, Games({}));
      CHECK(loc.dir == fs::path("/cli/game") && loc.why == su::GameDirWhy::kCommandLine);
    }
    {  // not portable: as before 1.13.1: a game folder next to the program that holds the game, else the shared default
      auto loc = su::ResolveGameLocation(In(l, false), Games({l.shared}));
      CHECK(loc.dir == fs::path(l.shared) && loc.why == su::GameDirWhy::kShared && loc.default_dir == fs::path(l.shared));
      loc = su::ResolveGameLocation(In(l, false), Games({}));
      CHECK(loc.dir == fs::path(l.shared) && loc.why == su::GameDirWhy::kShared);
      if (l.exe_dir_game[0]) {
        loc = su::ResolveGameLocation(In(l, false), Games({l.exe_dir_game, l.shared}));
        CHECK(loc.dir == fs::path(l.exe_dir_game) && loc.why == su::GameDirWhy::kNextToProgram);
      }
    }
    if (l.exe_dir_game[0]) {  // portable with a 1.12/1.13-style game folder next to the program: kept, no offer
      auto g = Games({l.exe_dir_game, l.shared});
      auto loc = su::ResolveGameLocation(In(l, true), g);
      CHECK(loc.dir == fs::path(l.exe_dir_game) && loc.why == su::GameDirWhy::kNextToProgram);
      CHECK(!su::ShouldOfferCopy(loc, false, g));
    }
    {  // the offer: portable, own folder empty, shared holds the game, not declined
      const auto yes = Games({l.shared});
      auto loc = su::ResolveGameLocation(In(l, true), yes);
      CHECK(su::ShouldOfferCopy(loc, false, yes));
      CHECK(!su::ShouldOfferCopy(loc, true, yes));  // Not now was chosen
      CHECK(su::CanCopyFromShared(true, loc.dir, loc.default_dir, loc.shared, yes));  // the page's button still shows
      CHECK(!su::ShouldOfferCopy(loc, false, Games({})));  // nothing to copy from
      auto own = Games({data_game.generic_string(), l.shared});
      loc = su::ResolveGameLocation(In(l, true), own);
      CHECK(!su::ShouldOfferCopy(loc, false, own));  // already has its own
      loc = su::ResolveGameLocation(In(l, false), yes);  // not portable: no offer, no button
      CHECK(!su::ShouldOfferCopy(loc, false, yes));
      CHECK(!su::CanCopyFromShared(false, loc.dir, loc.default_dir, loc.shared, yes));
      auto in = In(l, true);  // a chosen folder (even a missing one) or --game_data_root: no offer
      in.configured = "/elsewhere/game";
      CHECK(!su::ShouldOfferCopy(su::ResolveGameLocation(in, yes), false, yes));
      in.configured.clear();
      in.command_line = "/cli/game";
      CHECK(!su::ShouldOfferCopy(su::ResolveGameLocation(in, yes), false, yes));
    }
    // The player chose the shared folder in place: no button to copy (it is the one in use).
    CHECK(!su::CanCopyFromShared(true, l.shared, data_game, l.shared, Games({l.shared})));
  }
  {  // BOTS_DATA_DIR that is the shared folder's parent: one folder, nothing to copy
    su::GameLocationInput in;
    in.portable = true;
    in.data_root = "/x/bots-recomp";
    in.shared = "/x/bots-recomp/game";
    auto yes = Games({"/x/bots-recomp/game"});
    auto loc = su::ResolveGameLocation(in, yes);
    CHECK(loc.dir == fs::path("/x/bots-recomp/game") && !su::ShouldOfferCopy(loc, false, yes));
  }
  // Not now: an existing portable user keeps the shared folder; a brand-new portable folder gets the first-run state.
  CHECK(su::NotNowUsesShared(true) && !su::NotNowUsesShared(false));
  CHECK(std::string(su::WhyText(su::GameDirWhy::kPortableDefault)) == "portable default");
  CHECK(std::string(su::WhyText(su::GameDirWhy::kShared)) == "shared");
  CHECK(std::string(su::WhyText(su::GameDirWhy::kChosen)) == "chosen");

  // Space: the same margin an install keeps.
  CHECK(su::EnoughSpace((7ull << 30) + (256ull << 20), 7ull << 30));
  CHECK(!su::EnoughSpace((7ull << 30) + (255ull << 20), 7ull << 30));
  CHECK(su::SpaceMessage("/data", 6800000000ull, 2100000000ull) ==
        "Not enough free space in /data: needs 6.8 GB, 2.1 GB free.");

  // The copy, on real temp folders.
  const fs::path tmp = fs::temp_directory_path() / ("bots_gl_test_" + std::to_string(std::rand()));
  fs::remove_all(tmp);
  const fs::path src = tmp / "shared" / "game";
  fs::create_directories(src / "C1Game" / "Sub");
  auto put = [](const fs::path& p, size_t n, char c) { std::ofstream(p, std::ios::binary) << std::string(n, c); };
  put(src / "default.xex", 100, 'x');
  put(src / "C1Game" / "a.bin", 9 << 20, 'a');  // two chunks and a bit
  put(src / "C1Game" / "Sub" / "b.bin", 5, 'b');
  put(src / "C1Game" / "empty.bin", 0, 'e');
  const auto is_game = [](const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p / "default.xex", ec) && fs::is_directory(p / "C1Game", ec);
  };
  const su::GameSize size = su::MeasureGameDir(src);
  CHECK(size.error.empty() && size.files == 4 && size.bytes == 100 + (9u << 20) + 5);
  {  // success: copied, verified, swapped in; the source is untouched; no partial left
    const fs::path dest = tmp / "portable" / "bots-data" / "game";
    su::ExtractProgress progress;
    int verified = 0;
    auto r = su::CopyGameFiles(src, dest, progress, is_game, [&](const fs::path& p) {
      ++verified;
      return is_game(p) ? std::string() : std::string("bad");
    });
    CHECK(r.ok && r.error.empty());
    CHECK(verified == 1 && is_game(dest) && !fs::exists(dest.string() + ".partial"));
    CHECK(fs::file_size(dest / "C1Game" / "a.bin") == (9u << 20) && fs::exists(dest / "C1Game" / "empty.bin"));
    CHECK(progress.bytes_done == size.bytes && progress.files_done == 4 && progress.bytes_total == size.bytes);
    CHECK(is_game(src) && su::MeasureGameDir(src).bytes == size.bytes);  // copy, never move
    const fs::path empty = tmp / "portable2" / "game";  // an empty folder that already exists works too
    fs::create_directories(empty);
    su::ExtractProgress p2;
    CHECK(su::CopyGameFiles(src, empty, p2, is_game, {}).ok && is_game(empty));
  }
  {  // cancel after the first file: nothing installed, no partial
    const fs::path dest = tmp / "cancel" / "game";
    su::ExtractProgress progress;
    auto r = su::CopyGameFiles(src, dest, progress, is_game, {}, [&] { progress.cancel = true; });
    CHECK(!r.ok && r.error == "Cancelled.");
    CHECK(!fs::exists(dest) && !fs::exists(dest.string() + ".partial"));
  }
  {  // a failed check (the title id): the copy is thrown away
    const fs::path dest = tmp / "badtitle" / "game";
    su::ExtractProgress progress;
    auto r = su::CopyGameFiles(src, dest, progress, is_game, [](const fs::path&) { return std::string("not this game"); });
    CHECK(!r.ok && r.error == "not this game");
    CHECK(!fs::exists(dest) && !fs::exists(dest.string() + ".partial"));
  }
  {  // a file damaged in the copy: the size check catches it
    const fs::path dest = tmp / "mismatch" / "game";
    const fs::path partial = tmp / "mismatch" / "game.partial";
    su::ExtractProgress progress;
    bool damaged = false;
    auto r = su::CopyGameFiles(src, dest, progress, is_game, {}, [&] {
      std::error_code ec;
      for (fs::recursive_directory_iterator it(partial, ec), end; !ec && it != end && !damaged; it.increment(ec)) {
        if (it->is_regular_file() && it->file_size() > 1) fs::resize_file(it->path(), 1), damaged = true;
      }
    });
    CHECK(damaged && !r.ok && r.error.find("doesn't match the original") != std::string::npos);
    CHECK(!fs::exists(dest) && !fs::exists(partial));
  }
  {  // nothing to copy from
    su::ExtractProgress progress;
    auto r = su::CopyGameFiles(tmp / "nope", tmp / "x" / "game", progress, is_game, {});
    CHECK(!r.ok && !r.error.empty() && !fs::exists(tmp / "x" / "game"));
  }
  {  // "Not now" is remembered in the data folder
    const fs::path data = tmp / "decl";
    CHECK(!su::CopyDeclined(data));
    su::RememberCopyDeclined(data);
    CHECK(su::CopyDeclined(data) && !su::CopyDeclined(tmp / "other"));
  }
  fs::remove_all(tmp);
  if (failures == 0) std::printf("game_location_test: ok\n");
  return failures ? 1 : 0;
}
