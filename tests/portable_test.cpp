// Plain-assert tests for src/setup/portable.h (B-357). Run: c++ -std=c++20 -I src tests/portable_test.cpp -o /tmp/t && /tmp/t
// Paths are plain text here: nothing touches the real file system except the writable probe at the end.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "setup/portable.h"

namespace fs = std::filesystem;
namespace su = bots::setup;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// A pretend file system: the set of files that "exist".
static su::DataRoot Resolve(const std::string& env, const std::string& exe_dir, const std::string& appimage,
                            const std::set<std::string>& files, const std::string& cwd = "/cwd") {
  return su::ResolveDataRoot(env, exe_dir, appimage, cwd,
                             [&](const fs::path& p) { return files.count(p.generic_string()) > 0; });
}

int main() {
  // Windows folder and Linux tarball: portable.txt next to the executable.
  {
    auto r = Resolve("", "/games/Skullburglar", "", {"/games/Skullburglar/portable.txt"});
    CHECK(r.source == su::DataSource::kPortableFile);
    CHECK(r.root == fs::path("/games/Skullburglar/bots-data"));
    CHECK(r.marker == fs::path("/games/Skullburglar/portable.txt"));
    r = Resolve("", "/games/Skullburglar", "", {});
    CHECK(r.source == su::DataSource::kDefault && r.root.empty());
    // Not in the current directory: a shortcut starting elsewhere changes nothing.
    r = Resolve("", "/games/Skullburglar", "", {"/cwd/portable.txt"});
    CHECK(r.source == su::DataSource::kDefault);
  }
  // AppImage: the folder of the .AppImage file, not the read-only mount the executable runs from.
  {
    const std::set<std::string> f = {"/mnt/games/Games/portable.txt"};
    auto r = Resolve("", "/tmp/.mount_SkullXYZ/usr", "/mnt/games/Games/Skullburglar-1.12.AppImage", f);
    CHECK(r.source == su::DataSource::kPortableFile);
    CHECK(r.root == fs::path("/mnt/games/Games/bots-data"));
    // A portable.txt in the mount (it can't exist, but) is not looked at.
    r = Resolve("", "/tmp/.mount_SkullXYZ/usr", "/mnt/games/Games/Skullburglar-1.12.AppImage",
                {"/tmp/.mount_SkullXYZ/usr/portable.txt"});
    CHECK(r.source == su::DataSource::kDefault);
    // An AppImage given as a bare name has no folder part: fall back to the executable's folder.
    r = Resolve("", "/opt/s", "Skull.AppImage", {"/opt/s/portable.txt"});
    CHECK(r.source == su::DataSource::kPortableFile && r.root == fs::path("/opt/s/bots-data"));
  }
  // macOS: the folder holding the .app, never inside the bundle.
  {
    const std::string exe = "/Mac/p/Games/Skullburglar.app/Contents/MacOS";
    CHECK(su::IsBundleExeDir(exe));
    CHECK(!su::IsBundleExeDir("/Mac/p/Games/Contents/MacOS"));  // no .app above it
    CHECK(!su::IsBundleExeDir("/opt/skullburglar"));
    CHECK(su::PortableBaseDir(exe, "") == fs::path("/Mac/p/Games"));
    auto r = Resolve("", exe, "", {"/Mac/p/Games/portable.txt"});
    CHECK(r.source == su::DataSource::kPortableFile);
    CHECK(r.root == fs::path("/Mac/p/Games/bots-data"));
    // A portable.txt inside the bundle does not count.
    r = Resolve("", exe, "", {exe + "/portable.txt", "/Mac/p/Games/Skullburglar.app/portable.txt"});
    CHECK(r.source == su::DataSource::kDefault);
    CHECK(r.root.generic_string().find(".app") == std::string::npos);
  }
  // The environment variable beats portable.txt and the default; relative values are taken from the cwd.
  {
    auto r = Resolve("/data/bots", "/games/S", "", {"/games/S/portable.txt"});
    CHECK(r.source == su::DataSource::kEnv && r.root == fs::path("/data/bots"));
    r = Resolve("saves", "/games/S", "", {}, "/work");
    CHECK(r.source == su::DataSource::kEnv && r.root == fs::path("/work/saves"));
    r = Resolve("/data/./bots/../b2/", "/games/S", "", {});
    CHECK(r.root == fs::path("/data/b2/"));
    r = Resolve("", "/games/S", "", {"/games/S/portable.txt"});  // empty variable = unset
    CHECK(r.source == su::DataSource::kPortableFile);
  }
  // Layout: saves and settings under user/ so the backups and crash archives (siblings of user/) stay inside.
  {
    const su::DataLayout l = su::LayoutFor("/p/bots-data");
    CHECK(l.user == fs::path("/p/bots-data/user"));
    CHECK(l.cache == fs::path("/p/bots-data/cache"));
    CHECK(l.logs == fs::path("/p/bots-data/logs"));
  }
  // The writable probe: a real folder works and leaves nothing behind; a path under a file, and a read-only
  // folder (when not root), fail with a message that names the folder.
  {
    std::error_code ec;
    const fs::path t = fs::temp_directory_path() / ("bots_portable_test_" + std::to_string(std::rand()));
    fs::create_directories(t, ec);
    CHECK(su::CheckWritable(t / "a" / "b").empty());
    CHECK(fs::is_empty(t / "a" / "b"));
    std::ofstream(t / "file") << "x";
    const std::string err = su::CheckWritable(t / "file" / "sub");
    CHECK(!err.empty() && err.find("file") != std::string::npos);
#if !defined(_WIN32)
    fs::create_directories(t / "ro", ec);
    fs::permissions(t / "ro", fs::perms::owner_read | fs::perms::owner_exec, ec);
    if (!std::ofstream(t / "ro" / "probe")) {  // not running as root
      const std::string e2 = su::CheckWritable(t / "ro");
      CHECK(!e2.empty());
      su::DataRoot d;
      d.source = su::DataSource::kPortableFile;
      d.marker = "/x/portable.txt";
      CHECK(su::PortableErrorMessage(d, e2).find("portable.txt") != std::string::npos);
    }
    fs::permissions(t / "ro", fs::perms::owner_all, ec);
#endif
    fs::remove_all(t, ec);
  }
  // Misplaced data (B-386): saves and bots.toml copied into bots-data instead of bots-data/user.
  {
    using V = std::vector<std::string>;
    CHECK(su::IsGameDataName("bots.toml"));
    CHECK(su::IsGameDataName("5451083B") && su::IsGameDataName("B13EBABEBABEBABE") && su::IsGameDataName("5451083b"));
    CHECK(!su::IsGameDataName("cache") && !su::IsGameDataName("logs") && !su::IsGameDataName("user"));
    CHECK(!su::IsGameDataName("save-backups") && !su::IsGameDataName("crashes"));
    CHECK(!su::IsGameDataName("5451083") && !su::IsGameDataName("5451083G") && !su::IsGameDataName("B13EBABEBABEBABE0"));
    CHECK(!su::IsGameDataName("") && !su::IsGameDataName("Bots.toml"));
    // The reported case: a copy of Documents\bots put in bots-data, with user/ empty or missing.
    const V copied = {"bots.toml", "5451083B", "B13EBABEBABEBABE", "cache", "logs", "user"};
    CHECK((su::MisplacedGameData(copied, {}) == V{"bots.toml", "5451083B", "B13EBABEBABEBABE"}));
    CHECK((su::MisplacedGameData(copied, V{}) == V{"bots.toml", "5451083B", "B13EBABEBABEBABE"}));
    // user/ holds only things that aren't game data (a probe file, an empty folder): still flagged.
    CHECK(su::MisplacedGameData(copied, {".bots-write-test", "other"}).size() == 3);
    // user/ already has saves or settings: the player plays from there; nothing is flagged.
    CHECK(su::MisplacedGameData(copied, {"bots.toml"}).empty());
    CHECK(su::MisplacedGameData(copied, {"5451083B"}).empty());
    // A healthy portable folder, and an empty one.
    CHECK(su::MisplacedGameData({"user", "cache", "logs", "save-backups", "crashes"}, {"bots.toml", "5451083B"}).empty());
    CHECK(su::MisplacedGameData({}, {}).empty());
    // Only some of the entries are there.
    CHECK((su::MisplacedGameData({"5451083B", "logs"}, {}) == V{"5451083B"}));
    // The message names the folders and the entries.
    su::DataRoot d;
    d.source = su::DataSource::kPortableFile;
    d.root = "/g/bots-data";
    const std::string m = su::MisplacedMessage(d, {"bots.toml", "5451083B"});
    CHECK(m.find("bots.toml, 5451083B") != std::string::npos && m.find("user") != std::string::npos);
    // Probe and move on a real folder: the entries go into user/, nothing else moves, nothing is overwritten.
    std::error_code ec;
    const fs::path t = fs::temp_directory_path() / ("bots_misplaced_test_" + std::to_string(std::rand()));
    fs::create_directories(t / "bots-data" / "5451083B" / "profile", ec);
    fs::create_directories(t / "bots-data" / "cache", ec);
    std::ofstream(t / "bots-data" / "bots.toml") << "fullscreen = false\n";
    std::ofstream(t / "bots-data" / "5451083B" / "profile" / "p") << "save";
    d.root = t / "bots-data";
    CHECK((su::FindMisplacedGameData(d) == V{"bots.toml", "5451083B"} ||
           su::FindMisplacedGameData(d) == V{"5451083B", "bots.toml"}));
    std::string err;
    const V moved = su::MoveMisplacedGameData(d, su::FindMisplacedGameData(d), err);
    CHECK(moved.size() == 2 && err.empty());
    CHECK(fs::is_regular_file(t / "bots-data" / "user" / "bots.toml"));
    CHECK(fs::is_regular_file(t / "bots-data" / "user" / "5451083B" / "profile" / "p"));
    CHECK(!fs::exists(t / "bots-data" / "bots.toml") && !fs::exists(t / "bots-data" / "5451083B"));
    CHECK(fs::is_directory(t / "bots-data" / "cache"));
    CHECK(su::FindMisplacedGameData(d).empty());  // now healthy
    // A name already in user/ is not overwritten; a name that isn't game data is never moved.
    std::ofstream(t / "bots-data" / "bots.toml") << "new";
    std::ofstream(t / "bots-data" / "notes.txt") << "mine";
    err.clear();
    const V moved2 = su::MoveMisplacedGameData(d, {"bots.toml", "notes.txt"}, err);
    CHECK(moved2.empty() && !err.empty());
    CHECK(fs::exists(t / "bots-data" / "bots.toml") && fs::exists(t / "bots-data" / "notes.txt"));
    fs::remove_all(t, ec);
    // The default data location has no data folder to check.
    CHECK(su::FindMisplacedGameData(su::DataRoot{}).empty());
  }
  // B-389: portable mode that isn't on, and the save count for the log.
  {
    using V = std::vector<std::string>;
    CHECK(su::MarkerLookalike({"bots.exe", "portable.txt"}).empty());
    CHECK(su::MarkerLookalike({"bots.exe", "portable.txt.txt"}) == "portable.txt.txt");  // Windows hid the extension
    CHECK(su::MarkerLookalike({"portable"}) == "portable");
    CHECK(su::MarkerLookalike({"Portable.TXT"}) == "Portable.TXT");  // case-sensitive file systems
    CHECK(su::MarkerLookalike({"Portable.TXT", "portable.txt"}).empty());
    CHECK(su::MarkerLookalike({"bots.exe", "README.txt", "portable_notes.md"}).empty());
    CHECK(su::MarkerLookalike(V{}).empty());
    const fs::path base = "/g/Skullburglar", saves = "/data/Documents/bots";
    CHECK(su::MarkerHint("", false, base, saves).empty());
    const std::string a = su::MarkerHint("portable.txt.txt", false, base, saves);
    CHECK(a.find("portable.txt.txt") != std::string::npos && a.find("File name extensions") != std::string::npos);
    CHECK(a.find("/data/Documents/bots") != std::string::npos);
    const std::string b = su::MarkerHint("", true, base, saves);
    CHECK(b.find("bots-data folder is ignored") != std::string::npos && b.find("/g/Skullburglar") != std::string::npos);
    // The probe on a real folder.
    std::error_code ec;
    const fs::path t = fs::temp_directory_path() / ("bots_marker_test_" + std::to_string(std::rand()));
    fs::create_directories(t / "bots-data", ec);
    std::ofstream(t / "bots.exe") << "x";
    std::ofstream(t / "portable.txt.txt") << "";
    CHECK(su::FindMarkerHint(su::DataRoot{}, t, "", saves).find("portable.txt.txt") != std::string::npos);
    su::DataRoot on;
    on.source = su::DataSource::kPortableFile;
    CHECK(su::FindMarkerHint(on, t, "", saves).empty());  // portable mode is on: nothing to say
    fs::remove(t / "portable.txt.txt", ec);
    CHECK(su::FindMarkerHint(su::DataRoot{}, t, "", saves).find("bots-data folder is ignored") != std::string::npos);
    fs::remove_all(t / "bots-data", ec);
    CHECK(su::FindMarkerHint(su::DataRoot{}, t, "", saves).empty());  // a plain install: silence
    // Save count: one .header per saved game; the profile settings files and other folders don't count.
    const fs::path u = t / "user";
    fs::create_directories(u / "B13EBABEBABEBABE" / "5451083B" / "Headers" / "00000001", ec);
    fs::create_directories(u / "5451083B" / "profile" / "User", ec);
    fs::create_directories(u / "cache" / "x" / "Headers", ec);
    CHECK(su::CountSaves(u) == 0);
    std::ofstream(u / "B13EBABEBABEBABE" / "5451083B" / "Headers" / "00000001" / "default_checkpoint.sav.header") << "h";
    std::ofstream(u / "5451083B" / "profile" / "User" / "63E83FFF") << "p";
    CHECK(su::CountSaves(u) == 1);
    CHECK(su::CountSaves(t / "missing") == 0);
    fs::remove_all(t, ec);
  }
  std::printf(failures ? "portable_test: %d FAILED\n" : "portable_test: ok\n", failures);
  return failures ? 1 : 0;
}
