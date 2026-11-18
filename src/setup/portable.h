// Portable mode (B-357): where saves, settings (bots.toml), the shader cache and logs live.
//
// Resolution order: the BOTS_DATA_DIR environment variable, then a file named portable.txt next to the program, then the
// operating system's default folders. Command-line flags (--user_data_root, --cache_root, --log_file) still win over
// all three for the thing they name, which is what test and dev launches rely on.
//
// "Next to the program" means the program the player started, not the folder it was started from, so shortcuts and
// Steam launch entries keep working:
//   Windows, Linux tarball  the folder holding bots.exe / bots
//   Linux AppImage          the folder holding the .AppImage file ($APPIMAGE; the executable inside is in a
//                           read-only temporary mount)
//   macOS                   the folder holding Skullburglar.app (the bundle itself is signed: nothing is ever
//                           written inside it)
// The data goes in a folder named bots-data next to portable.txt, or in the BOTS_DATA_DIR folder itself, laid out as
//   user/    saves and bots.toml (the user data root)
//   cache/   the shader cache
//   logs/    one bots-<time>.log per launch
// plus save-backups/ and crashes/ (siblings of user/, see save_backup.h). Nothing here moves existing saves.
//
// Everything in this header is pure (no environment, no file system except through the callback), so
// tests/portable_test.cpp covers it on any host. The file-system probe at the end is separate.
#pragma once

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

#include "../path_utf8.h"

namespace bots::setup {

inline constexpr const char* kPortableMarker = "portable.txt";
inline constexpr const char* kPortableDirName = "bots-data";

enum class DataSource { kDefault, kEnv, kPortableFile };

struct DataRoot {
  DataSource source = DataSource::kDefault;
  std::filesystem::path root;    // the folder holding user/ cache/ logs/ (empty for kDefault)
  std::filesystem::path marker;  // the portable.txt that switched it on (kPortableFile only)
};

struct DataLayout {
  std::filesystem::path user;   // user data root: saves, bots.toml
  std::filesystem::path cache;  // shader cache
  std::filesystem::path logs;
};

// The executable inside a macOS bundle: Skullburglar.app/Contents/MacOS. Pure on the path text, so it can be tested
// on any host (app_paths.h's InAppBundle is the same test, but only true on macOS).
inline bool IsBundleExeDir(const std::filesystem::path& exe_dir) {
  return exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents" &&
         exe_dir.parent_path().parent_path().extension() == ".app";
}

// The folder portable.txt is looked up in (see the header comment). `appimage` is $APPIMAGE (empty when not set).
inline std::filesystem::path PortableBaseDir(const std::filesystem::path& exe_dir, std::string_view appimage) {
  if (!appimage.empty()) {
    const std::filesystem::path parent = bots::PathFromUtf8(appimage).parent_path();
    if (!parent.empty()) return parent;
  }
  if (IsBundleExeDir(exe_dir)) return exe_dir.parent_path().parent_path().parent_path();
  return exe_dir;
}

// Picks the data root. `env_value` is BOTS_DATA_DIR (empty or unset: ignored). `is_file` says whether a path is an
// existing regular file. A relative BOTS_DATA_DIR is taken relative to `cwd`.
inline DataRoot ResolveDataRoot(std::string_view env_value, const std::filesystem::path& exe_dir,
                                std::string_view appimage, const std::filesystem::path& cwd,
                                const std::function<bool(const std::filesystem::path&)>& is_file) {
  DataRoot out;
  if (!env_value.empty()) {
    std::filesystem::path p = bots::PathFromUtf8(env_value);
    if (p.is_relative()) p = cwd / p;
    out.source = DataSource::kEnv;
    out.root = p.lexically_normal();
    return out;
  }
  if (exe_dir.empty() && appimage.empty()) return out;
  const std::filesystem::path base = PortableBaseDir(exe_dir, appimage);
  const std::filesystem::path marker = base / kPortableMarker;
  if (is_file && is_file(marker)) {
    out.source = DataSource::kPortableFile;
    out.root = base / kPortableDirName;
    out.marker = marker;
  }
  return out;
}

inline DataLayout LayoutFor(const std::filesystem::path& root) {
  return DataLayout{root / "user", root / "cache", root / "logs"};
}

// Creates `dir` if needed and proves it can be written by making and removing a probe file. Empty when it works,
// else a sentence for the player. No fallback: a portable folder that can't be written is an error, not a reason to
// save somewhere the player didn't choose.
inline std::string CheckWritable(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec || !fs::is_directory(dir, ec)) {
    return "Can't create the folder " + bots::PlainPathUtf8(dir) + " (" + (ec ? ec.message() : "not a folder") + ").";
  }
  const fs::path probe = dir / ".bots-write-test";
  if (std::FILE* f = bots::OpenFile(probe, "wb")) {
    std::fclose(f);
    fs::remove(probe, ec);
    return {};
  }
  return "Can't write to the folder " + bots::PlainPathUtf8(dir) + ".";
}

// What this run resolved to (set once by the app's OnConfigurePaths; the launcher and the log read it).
inline DataRoot& ActiveDataRoot() {
  static DataRoot active;
  return active;
}

// The message for the player when portable mode can't be used. `why` is CheckWritable's text.
inline std::string PortableErrorMessage(const DataRoot& data, const std::string& why) {
  std::string m = "Portable mode is on, but the data folder can't be used.\n\n" + why + "\n\n";
  if (data.source == DataSource::kEnv) {
    m += "The BOTS_DATA_DIR setting points there. Choose a folder you can write to, or remove the setting.";
  } else {
    m += "portable.txt (" + bots::PlainPathUtf8(data.marker) +
         ") keeps the saves, settings, shader cache and logs next to it. Move the game to a folder you can write to, "
         "or delete portable.txt to use your user folder again.";
  }
  return m;
}

}  // namespace bots::setup
