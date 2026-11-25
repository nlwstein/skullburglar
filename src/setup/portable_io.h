// Portable mode never writes outside its own folder (B-408). The data folder (portable.h: user/, cache/, logs/, ...) is
// where the game puts its own files; this header covers everything else that creates files for us:
//   scratch files   update downloads and staging, the disk image mount point, anything made with the system's temp
//                   folder: TMPDIR (macOS, Linux) or TEMP and TMP (Windows) point at <data>/tmp for this process and
//                   everything it starts
//   preferences     macOS: the key-repeat setting is made for this process only (REX_NO_PREFS_WRITE=1, SDK patch 0040)
//   driver caches   Mesa and NVIDIA keep compiled shaders under $XDG_CACHE_HOME (default ~/.cache) or the path in
//                   MESA_SHADER_CACHE_DIR / __GL_SHADER_DISK_CACHE_PATH, and the audio library keeps a runtime link
//                   under $XDG_CONFIG_HOME (default ~/.config): they point at <data>/cache/drivers and <data>/cache/xdg
// The one exception to "nothing outside" is the copy or move of existing game files out of the default location, when
// the player asks for it (game_location.h). What no setting can move is listed in docs/FILES.md ("What portable mode
// can't move").
//
// Pure: the callers apply the list. tests/portable_io_test.cpp covers it on any host.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../path_utf8.h"

namespace bots::setup {

enum class HostOs { kWindows, kMac, kLinux };

struct EnvAssignment {
  std::string name;
  std::string value;  // UTF-8
  bool operator==(const EnvAssignment& o) const { return name == o.name && value == o.value; }
};

inline constexpr const char* kScratchDirName = "tmp";
inline constexpr const char* kDriverCacheDirName = "drivers";

// The scratch folder of a portable data folder.
inline std::filesystem::path ScratchDir(const std::filesystem::path& data_root) { return data_root / kScratchDirName; }

// The folder driver caches go to.
inline std::filesystem::path DriverCacheDir(const std::filesystem::path& data_root) {
  return data_root / "cache" / kDriverCacheDirName;
}

// What a portable run sets in its own environment before anything else starts (see the header comment). Every value is
// inside `data_root`, so a test can check that none of them points anywhere else.
inline std::vector<EnvAssignment> PortableEnvironment(const std::filesystem::path& data_root, HostOs os) {
  std::vector<EnvAssignment> out;
  const std::string tmp = bots::PathUtf8(ScratchDir(data_root));
  if (os == HostOs::kWindows) {
    out.push_back({"TEMP", tmp});
    out.push_back({"TMP", tmp});
  } else {
    out.push_back({"TMPDIR", tmp});
  }
  const std::filesystem::path drivers = DriverCacheDir(data_root);
  if (os == HostOs::kLinux) {
    // The XDG folders: caches (Mesa, NVIDIA, fontconfig) and config (PulseAudio's client makes ~/.config/pulse/<id>-runtime
    // when SDL opens the audio device, even with no server of its own).
    const std::filesystem::path xdg = data_root / "cache" / "xdg";
    out.push_back({"XDG_CACHE_HOME", bots::PathUtf8(xdg / "cache")});
    out.push_back({"XDG_CONFIG_HOME", bots::PathUtf8(xdg / "config")});
  }
  out.push_back({"MESA_SHADER_CACHE_DIR", bots::PathUtf8(drivers / "mesa")});
  out.push_back({"__GL_SHADER_DISK_CACHE_PATH", bots::PathUtf8(drivers / "nvidia")});
  return out;
}

// The folders the environment above names, to create before the process uses them.
inline std::vector<std::filesystem::path> PortableEnvironmentDirs(const std::filesystem::path& data_root, HostOs os) {
  std::vector<std::filesystem::path> out;
  for (const auto& a : PortableEnvironment(data_root, os)) out.push_back(bots::PathFromUtf8(a.value));
  return out;
}

// True when `path` is `root` or inside it (lexically; both should be absolute).
inline bool IsInside(const std::filesystem::path& path, const std::filesystem::path& root) {
  const auto p = path.lexically_normal(), r = root.lexically_normal();
  auto pi = p.begin();
  for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi) {
    if (ri->empty()) continue;  // a trailing separator
    if (pi == p.end() || *pi != *ri) return false;
  }
  return true;
}

// BOTS_PORTABLE_ISOLATE=0 turns the whole of B-408 off (a way back if it breaks someone's setup, and how the isolation
// test shows what the same build leaked before). On by default.
inline bool PortableIsolationOn() {
  const char* v = std::getenv("BOTS_PORTABLE_ISOLATE");
  return !(v && v[0] == '0');
}

// The running host.
inline constexpr HostOs CurrentHostOs() {
#if defined(_WIN32)
  return HostOs::kWindows;
#elif defined(__APPLE__)
  return HostOs::kMac;
#else
  return HostOs::kLinux;
#endif
}

// Creates the folders and sets the variables, for this process and everything it starts. Call before the first
// library that reads them runs (the graphics driver, the SDK). False if a folder can't be made: the caller already
// proved the data folder is writable (CheckWritable), so this only fails on a full disk.
inline bool ApplyPortableEnvironment(const std::filesystem::path& data_root, HostOs os = CurrentHostOs()) {
  if (!PortableIsolationOn()) return true;
  bool ok = true;
  for (const auto& dir : PortableEnvironmentDirs(data_root, os)) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) ok = false;
  }
  for (const auto& a : PortableEnvironment(data_root, os)) {
#if defined(_WIN32)
    ok = _putenv_s(a.name.c_str(), a.value.c_str()) == 0 && ok;
#else
    ok = setenv(a.name.c_str(), a.value.c_str(), 1) == 0 && ok;
#endif
  }
#if !defined(_WIN32)
  // macOS: the SDK sets "ApplePressAndHoldEnabled = NO" with CFPreferences, which writes ~/Library/Preferences/<program>.plist
  // wherever HOME points; with this set it does the same for this process only, in memory (SDK patch 0040).
  if (os == HostOs::kMac) ok = setenv("REX_NO_PREFS_WRITE", "1", 1) == 0 && ok;
#endif
  return ok;
}

// Whether the Windows setup program may update this install. It writes the Start menu entry, the uninstall entry and
// a pending-update note in %LOCALAPPDATA%, none of which a portable folder can hold, so portable mode leaves updating
// to the player (the "Download it" button). Empty when there is no objection.
inline std::string PortableUpdateBlocker(bool portable, bool windows_setup_install) {
  if (portable && windows_setup_install && PortableIsolationOn())
    return "Portable mode keeps everything in its own folder, and the Windows setup program writes outside it (the "
           "Start menu, the uninstall entry). Download the new version instead.";
  return {};
}

// Whether checking if an update could run must avoid creating files outside the folder the program was started from
// (B-408): true in portable mode with isolation on. The check then reads permissions; the real swap tries the rest.
inline bool PortableMayNotProbeOutside(bool portable) { return portable && PortableIsolationOn(); }

}  // namespace bots::setup
