// Plain-assert tests for src/setup/portable_io.h (B-408): where a portable run sends scratch files and driver caches.
// Run: c++ -std=c++20 -Wall -Werror -I src tests/portable_io_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "setup/portable_io.h"

namespace fs = std::filesystem;
namespace su = bots::setup;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static bool Has(const std::vector<su::EnvAssignment>& v, const std::string& name) {
  for (const auto& a : v) if (a.name == name) return true;
  return false;
}

int main() {
  const fs::path root = "/games/Skullburglar/bots-data";
  // Every value points inside the data folder, on every host.
  for (const su::HostOs os : {su::HostOs::kWindows, su::HostOs::kMac, su::HostOs::kLinux}) {
    for (const auto& a : su::PortableEnvironment(root, os)) {
      CHECK(!a.value.empty());
      CHECK(su::IsInside(bots::PathFromUtf8(a.value), root));
    }
    for (const auto& d : su::PortableEnvironmentDirs(root, os)) CHECK(su::IsInside(d, root));
  }
  // Temp: TMPDIR on POSIX, TEMP and TMP on Windows.
  CHECK(Has(su::PortableEnvironment(root, su::HostOs::kLinux), "TMPDIR"));
  CHECK(Has(su::PortableEnvironment(root, su::HostOs::kMac), "TMPDIR"));
  CHECK(!Has(su::PortableEnvironment(root, su::HostOs::kWindows), "TMPDIR"));
  CHECK(Has(su::PortableEnvironment(root, su::HostOs::kWindows), "TEMP"));
  CHECK(Has(su::PortableEnvironment(root, su::HostOs::kWindows), "TMP"));
  CHECK(!Has(su::PortableEnvironment(root, su::HostOs::kLinux), "TEMP"));
  // The XDG cache only moves on Linux (macOS and Windows have no such convention).
  CHECK(Has(su::PortableEnvironment(root, su::HostOs::kLinux), "XDG_CACHE_HOME"));
  CHECK(Has(su::PortableEnvironment(root, su::HostOs::kLinux), "XDG_CONFIG_HOME"));
  CHECK(!Has(su::PortableEnvironment(root, su::HostOs::kMac), "XDG_CONFIG_HOME"));
  CHECK(!Has(su::PortableEnvironment(root, su::HostOs::kWindows), "XDG_CONFIG_HOME"));
  CHECK(!Has(su::PortableEnvironment(root, su::HostOs::kMac), "XDG_CACHE_HOME"));
  CHECK(Has(su::PortableEnvironment(root, su::HostOs::kWindows), "__GL_SHADER_DISK_CACHE_PATH"));
  // The data-folder layout is the one portable.h documents: scratch in tmp/, driver caches under cache/.
  CHECK(su::ScratchDir(root) == root / "tmp");
  CHECK(su::DriverCacheDir(root) == root / "cache" / "drivers");

  // IsInside: whole components, not string prefixes.
  CHECK(su::IsInside("/a/b/c", "/a/b"));
  CHECK(su::IsInside("/a/b", "/a/b"));
  CHECK(su::IsInside("/a/b/", "/a/b"));
  CHECK(!su::IsInside("/a/bc", "/a/b"));
  CHECK(!su::IsInside("/a", "/a/b"));
  CHECK(!su::IsInside("/a/b/../c", "/a/b"));
  CHECK(su::IsInside("/a/b/../b/c", "/a/b"));

  // The Windows setup program is the one self-update that writes outside the folder.
  CHECK(!su::PortableUpdateBlocker(true, true).empty());
  CHECK(su::PortableUpdateBlocker(false, true).empty());
  CHECK(su::PortableUpdateBlocker(true, false).empty());

  // Asking whether an update could run never creates files outside the folder in portable mode.
  CHECK(su::PortableMayNotProbeOutside(true));
  CHECK(!su::PortableMayNotProbeOutside(false));

  // Applying it to this process creates the folders and sets the variables.
  {
    const fs::path tmp = fs::temp_directory_path() / ("bots-portable-io-test-" + std::to_string(std::rand()));
    CHECK(su::ApplyPortableEnvironment(tmp / "bots-data"));
    CHECK(fs::is_directory(tmp / "bots-data" / "tmp"));
    CHECK(fs::is_directory(tmp / "bots-data" / "cache" / "drivers"));
#if !defined(_WIN32)
    CHECK(std::string(std::getenv("TMPDIR")) == bots::PathUtf8(tmp / "bots-data" / "tmp"));
    CHECK(fs::temp_directory_path() == tmp / "bots-data" / "tmp");  // what the updater's staging uses
#endif
    std::error_code ec;
    fs::remove_all(tmp, ec);
  }

  std::printf(failures ? "%d FAILED\n" : "portable_io_test: all passed\n", failures);
  return failures ? 1 : 0;
}
