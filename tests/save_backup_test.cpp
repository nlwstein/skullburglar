// Plain-assert tests for src/save_backup.h. Run: c++ -std=c++20 -I src tests/save_backup_test.cpp -o /tmp/t && /tmp/t
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "save_backup.h"

namespace fs = std::filesystem;
namespace sb = bots::savebackup;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void Write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}
static std::string Read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
static std::string Stamp(int n) {  // 20260101-000000 + n seconds, as a name
  char b[32];
  std::snprintf(b, sizeof(b), "20260101-%02d%02d%02d", n / 3600, (n / 60) % 60, n % 60);
  return b;
}

int main() {
  // Names: the timestamp form, and what counts as one.
  CHECK(sb::IsStamp("20261103-141503"));
  CHECK(!sb::IsStamp("20261103-14150"));
  CHECK(!sb::IsStamp("20261103_141503"));
  CHECK(!sb::IsStamp("2026110a-141503"));
  CHECK(!sb::IsStamp("20261103-141503.partial"));
  CHECK(sb::IsStamp(sb::Timestamp(1793715303)));

  // Where the folders go.
  CHECK(sb::SiblingDir("/x/run/user", "save-backups") == fs::path("/x/run/save-backups"));
  CHECK(sb::SiblingDir("/x/run/user/", "crashes") == fs::path("/x/run/crashes"));
  CHECK(sb::SiblingDir("/data/p/.local/share/bots", "save-backups") == fs::path("/data/p/.local/share/bots-save-backups"));
  CHECK(sb::SiblingDir("/data/p/Documents/bots", "crashes") == fs::path("/data/p/Documents/bots-crashes"));

  // Pruning: the oldest go, the newest 10 stay, other names are never listed.
  {
    std::vector<std::string> names;
    for (int i : {5, 1, 12, 3, 0, 11, 7, 2, 9, 4, 6, 8}) names.push_back(Stamp(i));  // 12 shuffled
    names.push_back("notes.txt");
    names.push_back(Stamp(0) + ".partial");
    const auto drop = sb::PruneList(names);
    CHECK(drop.size() == 2);
    CHECK(drop.size() == 2 && drop[0] == Stamp(0) && drop[1] == Stamp(1));
    CHECK(sb::PruneList({Stamp(1), Stamp(2)}).empty());
    CHECK(sb::PruneList({}).empty());
    CHECK(sb::PruneList(names, 12).size() == 0);
  }

  {
    const fs::path d = fs::temp_directory_path() / "save_backup_test_logs";
    fs::remove_all(d);
    CHECK(sb::NewestLog(d, "bots_").empty());
    Write(d / "bots_001.log", "a");
    Write(d / "bots_012.log", "b");
    Write(d / "bots_009.log", "c");
    Write(d / "other_099.log", "d");
    CHECK(sb::NewestLog(d, "bots_") == d / "bots_012.log");
    fs::remove_all(d);
  }

  const fs::path base = fs::temp_directory_path() / "save_backup_test_tmp";
  fs::remove_all(base);
  const fs::path root = base / "run" / "user";

  // Nothing to back up: no folder, only a cache, only settings.
  CHECK(!sb::HasSaves(root));
  CHECK(sb::BackUp(root, base / "run" / "save-backups", Stamp(0)).empty());
  Write(root / "cache" / "shaders.bin", "big");
  Write(root / "bots.toml", "x = 1\n");
  CHECK(!sb::HasSaves(root));
  CHECK(sb::BackUp(root, base / "run" / "save-backups", Stamp(0)).empty());
  CHECK(!fs::exists(base / "run" / "save-backups"));  // not even the folder

  // With a save: copied without the cache, same bytes, live save untouched.
  Write(root / "5451083B" / "profile" / "User" / "63E83FFD", "profile-bytes");
  Write(root / "B13EBABEBABEBABE" / "default_checkpoint.sav", std::string(5000, 'S'));
  CHECK(sb::HasSaves(root));
  const fs::path bdir = base / "run" / "save-backups";
  std::string err;
  const fs::path first = sb::BackUp(root, bdir, Stamp(0), &err);
  CHECK(!first.empty() && err.empty());
  CHECK(Read(first / "B13EBABEBABEBABE" / "default_checkpoint.sav") == std::string(5000, 'S'));
  CHECK(Read(first / "5451083B" / "profile" / "User" / "63E83FFD") == "profile-bytes");
  CHECK(Read(first / "bots.toml") == "x = 1\n");
  CHECK(!fs::exists(first / "cache"));
  CHECK(Read(root / "B13EBABEBABEBABE" / "default_checkpoint.sav") == std::string(5000, 'S'));
  CHECK(fs::exists(root / "cache" / "shaders.bin"));
  // The Windows minidump path: logs/crash-<pid>.dmp beside the executable, absolute, whatever the working directory.
  {
    const fs::path exe_dir = fs::path("C:/Games/Skull burglar") / bots::PathFromUtf8("caf\xC3\xA9");
    CHECK(sb::DumpPath(exe_dir, 4242) == exe_dir / "logs" / "crash-4242.dmp");
    CHECK(sb::DumpPath(exe_dir, 7).parent_path() == exe_dir / "logs");
    CHECK(sb::DumpPath(exe_dir, 7).is_absolute() == exe_dir.is_absolute());
    CHECK(bots::PathUtf8(sb::DumpPath(exe_dir, 7).parent_path().parent_path().filename()) == "caf\xC3\xA9");
  }

  // The same second again: no second copy.
  CHECK(sb::BackUp(root, bdir, Stamp(0)).empty());

  // Fifteen launches keep ten, the newest ones, and a stray folder survives.
  fs::create_directories(bdir / "keep-me");
  for (int i = 1; i < 15; ++i) CHECK(!sb::BackUp(root, bdir, Stamp(i)).empty());
  std::vector<std::string> left;
  for (const auto& e : fs::directory_iterator(bdir)) left.push_back(e.path().filename().string());
  std::sort(left.begin(), left.end());
  CHECK(left.size() == 11);  // 10 backups + keep-me
  CHECK(left.size() == 11 && left[0] == Stamp(5) && left[1] == Stamp(6) && left[10] == "keep-me");
  CHECK(Read(root / "B13EBABEBABEBABE" / "default_checkpoint.sav") == std::string(5000, 'S'));

  // A failure is reported, leaves no partial folder and doesn't throw.
  Write(base / "blocker", "a file where the backup folder should be");
  err.clear();
  CHECK(sb::BackUp(root, base / "blocker", Stamp(30), &err).empty());
  CHECK(!err.empty());

  // Crash archive: the files that exist are copied, the rest skipped.
  Write(base / "logs" / "bots_001.log", "log text");
  Write(base / "logs" / "crash-42.dmp", "dump");
  const fs::path cdir = base / "run" / "crashes";
  CHECK(sb::ArchiveCrash({base / "logs" / "bots_001.log", base / "logs" / "crash-42.dmp", base / "logs" / "missing.txt"},
                         cdir, Stamp(40)) == 2);
  CHECK(Read(cdir / Stamp(40) / "crash-42.dmp") == "dump");
  CHECK(Read(cdir / Stamp(40) / "bots_001.log") == "log text");
  for (int i = 41; i < 53; ++i) sb::ArchiveCrash({base / "logs" / "bots_001.log"}, cdir, Stamp(i));
  sb::Prune(cdir);
  int n = 0;
  for (const auto& e : fs::directory_iterator(cdir)) { (void)e; ++n; }
  CHECK(n == 10);
  CHECK(!fs::exists(cdir / Stamp(40)) && fs::exists(cdir / Stamp(52)));

#if !defined(_WIN32)
  // The signal-handler path: prepared at startup, writes only when a crash calls it.
  sb::Arm(base / "run" / "crashes", Stamp(60), base / "logs" / "bots_001.log");
  CHECK(!fs::exists(base / "run" / "crashes" / Stamp(60)));
  const int fd = sb::OpenBacktrace();
  CHECK(fd >= 0);
  if (fd >= 0) {
    const char bt[] = "frame0\n";
    CHECK(::write(fd, bt, sizeof(bt) - 1) == static_cast<ssize_t>(sizeof(bt) - 1));
    ::close(fd);
  }
  sb::CopyLog();
  CHECK(Read(base / "run" / "crashes" / Stamp(60) / "backtrace.txt") == "frame0\n");
  CHECK(Read(base / "run" / "crashes" / Stamp(60) / "bots_001.log") == "log text");
#endif

  fs::remove_all(base);
  if (failures == 0) std::printf("save_backup_test: all passed\n");
  return failures ? 1 : 0;
}
