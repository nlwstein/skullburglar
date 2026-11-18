// Plain-assert tests for src/setup/portable.h (B-357). Run: c++ -std=c++20 -I src tests/portable_test.cpp -o /tmp/t && /tmp/t
// Paths are plain text here: nothing touches the real file system except the writable probe at the end.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

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
  std::printf(failures ? "portable_test: %d FAILED\n" : "portable_test: ok\n", failures);
  return failures ? 1 : 0;
}
