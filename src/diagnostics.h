// Player diagnostics (B-390, B-403): what the log says about the machine and the settings a run used, so a report
// doesn't stall on "which CPU?", "did your .bat switch take effect?", "is the game folder on a link?".
//
// Two things go to the log once at startup:
//   env: NAME=value        every BOTS_* and REX_* environment variable that is set, values shortened and secrets hidden
//   summary: key: value    a short block (OS, CPU, memory, how the game was started, folders, display, HDR, controllers);
//                          the "summary: " prefix is stable, so `grep '^.*summary: '` pulls the block out of any log
// and the launcher's About & logs page can save the block and the newest logs to one file (SaveBundle).
//
// This header is the pure part (parsing, sanitizing, formatting) and has no SDK or OS dependencies: tests/diagnostics_test.cpp
// builds it alone. The probes that ask the OS are in diagnostics.cpp.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace bots::diag {

inline std::string Upper(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

// Names that could hold a credential: their value is never logged.
inline bool IsSecretName(std::string_view name) {
  const std::string u = Upper(name);
  for (const char* word : {"TOKEN", "KEY", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL", "COOKIE"})
    if (u.find(word) != std::string::npos) return true;
  return false;
}

inline bool LooksLikePath(std::string_view v) {
  if (v.find('/') != std::string_view::npos || v.find('\\') != std::string_view::npos) return true;
  return v.size() > 2 && v[1] == ':' && std::isalpha(static_cast<unsigned char>(v[0]));
}

// A path for a log line: the home folder becomes "~", and a long path keeps its last two parts ("...\Games\Bots").
inline std::string ShortenPath(std::string path, std::string_view home = {}, size_t keep_whole = 64) {
  if (!home.empty() && home.size() > 1 && path.compare(0, home.size(), home) == 0 &&
      (path.size() == home.size() || path[home.size()] == '/' || path[home.size()] == '\\'))
    path = "~" + path.substr(home.size());
  if (path.size() <= keep_whole) return path;
  const char sep = path.find('\\') != std::string::npos && path.find('/') == std::string::npos ? '\\' : '/';
  size_t cut = path.size();
  for (int parts = 0; parts < 2 && cut != std::string::npos && cut > 0; ++parts) cut = path.rfind(sep, cut - 1);
  if (cut == std::string::npos || cut == 0) return "..." + path.substr(path.size() - keep_whole / 2);
  return std::string("...") + path.substr(cut);
}

// One environment value as it goes in the log: hidden when the name says secret, shortened when it is a path, cut at
// `max_len`, and "(empty)" when empty. Control characters become spaces so a value can't forge a log line.
inline std::string SanitizeEnvValue(std::string_view name, std::string_view value, std::string_view home = {},
                                    size_t max_len = 120) {
  if (IsSecretName(name)) return "<hidden>";
  if (value.empty()) return "(empty)";
  std::string v(value);
  for (char& c : v)
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  if (LooksLikePath(v)) v = ShortenPath(v, home);
  if (v.size() > max_len) v = v.substr(0, max_len) + "...";
  return v;
}

// Whether a variable is one of ours to log.
inline bool IsOurSwitch(std::string_view name) {
  return name.rfind("BOTS_", 0) == 0 || name.rfind("REX_", 0) == 0;
}

// "env: NAME=value" for every BOTS_* / REX_* entry of an environment block ("NAME=value" strings), sorted by name.
inline std::vector<std::string> EnvSwitchLines(const std::vector<std::string>& environment, std::string_view home = {}) {
  std::map<std::string, std::string> found;
  for (const std::string& entry : environment) {
    const size_t eq = entry.find('=', 1);  // Windows keeps entries such as "=C:=C:\" (a name starting with '=')
    if (eq == std::string::npos) continue;
    const std::string name = entry.substr(0, eq);
    if (IsOurSwitch(name)) found[name] = SanitizeEnvValue(name, std::string_view(entry).substr(eq + 1), home);
  }
  std::vector<std::string> lines;
  for (const auto& [name, value] : found) lines.push_back("env: " + name + "=" + value);
  return lines;
}

// KEY=value or KEY="value" lines (/etc/os-release, Steam's version files); comments and blank lines skipped.
inline std::map<std::string, std::string> ParseKeyValues(std::string_view text) {
  std::map<std::string, std::string> kv;
  std::istringstream in{std::string(text)};
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] == '#') continue;
    const size_t eq = line.find('=', start);
    if (eq == std::string::npos) continue;
    std::string value = line.substr(eq + 1);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
      value = value.substr(1, value.size() - 2);
    kv[line.substr(start, eq - start)] = value;
  }
  return kv;
}

// "SteamOS 3.7.8 (steamdeck, build 20250211.1)" from /etc/os-release; plain "Ubuntu 24.04.1 LTS" elsewhere.
inline std::string LinuxOsName(const std::map<std::string, std::string>& os) {
  auto get = [&](const char* k) {
    const auto it = os.find(k);
    return it == os.end() ? std::string() : it->second;
  };
  std::string name = !get("PRETTY_NAME").empty() ? get("PRETTY_NAME") : get("NAME");
  if (name.empty()) name = "Linux";
  std::string extra;
  for (const char* k : {"VARIANT_ID", "BUILD_ID"}) {
    if (get(k).empty()) continue;
    extra += (extra.empty() ? "" : ", ") + std::string(k == std::string_view("BUILD_ID") ? "build " : "") + get(k);
  }
  return extra.empty() ? name : name + " (" + extra + ")";
}

// Octal escapes in /proc/self/mountinfo paths ("\040" is a space).
inline std::string UnescapeMountPath(std::string_view s) {
  std::string r;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() + 0 && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 2])) && std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
      r += char(((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) | (s[i + 3] - '0'));
      i += 3;
    } else {
      r += s[i];
    }
  }
  return r;
}

// The filesystem type of the mount holding `path` (an absolute, resolved path), from /proc/self/mountinfo text: the
// longest mount point that is a prefix of the path. Empty if nothing matches.
inline std::string FsTypeFromMountInfo(std::string_view mountinfo, std::string_view path) {
  std::string best_type;
  size_t best_len = 0;
  std::istringstream in{std::string(mountinfo)};
  for (std::string line; std::getline(in, line);) {
    // id parent major:minor root mount-point options [optional fields...] - fstype source super-options
    std::istringstream f(line);
    std::string id, parent, dev, root, mount;
    if (!(f >> id >> parent >> dev >> root >> mount)) continue;
    const size_t dash = line.find(" - ");
    if (dash == std::string::npos) continue;
    std::istringstream tail(line.substr(dash + 3));
    std::string type;
    if (!(tail >> type)) continue;
    mount = UnescapeMountPath(mount);
    const bool prefix = path.size() >= mount.size() && path.compare(0, mount.size(), mount) == 0 &&
                        (mount == "/" || path.size() == mount.size() || path[mount.size()] == '/');
    if (prefix && mount.size() >= best_len) best_len = mount.size(), best_type = type;
  }
  return best_type;
}

// What the file system name means to a player: "ntfs3" and "fuseblk" (ntfs-3g) are NTFS, and so on.
inline std::string FsTypeNote(std::string type) {
  if (type == "fuseblk") return "fuseblk (NTFS or exFAT through FUSE)";
  if (type == "ntfs3") return "ntfs";
  return type;
}

// How the game was started, from the environment: a Steam Deck in Game Mode runs the game in gamescope with the Steam
// overlay and Steam Input; Desktop Mode has SteamDeck=1 and none of that. `getenv` returns "" for unset.
inline std::string LaunchContext(const std::function<std::string(const char*)>& getenv_fn) {
  auto set = [&](const char* n) { return !getenv_fn(n).empty(); };
  const bool deck = getenv_fn("SteamDeck") == "1" || set("SteamDeck");
  const bool game_mode = getenv_fn("SteamGamepadUI") == "1" || set("SteamGamepadUI");
  const bool gamescope = set("GAMESCOPE_WAYLAND_DISPLAY") || set("GAMESCOPE_XWAYLAND_DISPLAY") ||
                         set("GAMESCOPE_CURSOR_X") || getenv_fn("XDG_CURRENT_DESKTOP") == "gamescope" ||
                         getenv_fn("XDG_SESSION_DESKTOP") == "gamescope";
  std::vector<std::string> parts;
  if (deck) parts.push_back(game_mode ? "Steam Deck in Game Mode" : "Steam Deck in Desktop Mode");
  else if (game_mode) parts.push_back("Steam Gaming Mode (not a Deck)");
  if (gamescope) parts.push_back("under gamescope");
  if (set("SteamAppId") || set("SteamGameId")) parts.push_back("started from Steam (app " + (set("SteamAppId") ? getenv_fn("SteamAppId") : getenv_fn("SteamGameId")) + ")");
  else if (set("SteamClientLaunch") || set("STEAM_COMPAT_CLIENT_INSTALL_PATH")) parts.push_back("started from Steam");
  if (set("STEAM_COMPAT_DATA_PATH") || set("WINEPREFIX")) parts.push_back("under Proton/Wine");
  if (set("SteamNoOverlayUI")) parts.push_back("Steam overlay off");
  if (parts.empty()) return "plain desktop session (not started from Steam)";
  std::string r;
  for (size_t i = 0; i < parts.size(); ++i) r += (i ? ", " : "") + parts[i];
  return r;
}

// How this copy was installed, from where the program is and a few environment hints.
inline std::string InstallKind(std::string_view exe_path, const std::function<std::string(const char*)>& getenv_fn) {
  auto contains = [&](std::string_view part) { return exe_path.find(part) != std::string_view::npos; };
  if (!getenv_fn("APPIMAGE").empty() || contains("/.mount_") || contains(".AppImage")) return "AppImage";
  if (contains(".app/Contents/MacOS")) return "macOS app bundle";
  if (contains("/steamapps/")) return "Steam library";
  if (contains("\\Program Files") || contains("/usr/") || contains("/opt/")) return "installed system-wide";
  if (contains("\\WindowsApps\\") || contains("/flatpak/") || !getenv_fn("FLATPAK_ID").empty()) return "packaged app";
  return "unpacked download (zip or tar.gz folder)";
}

inline std::string GiB(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f GiB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return buf;
}

inline std::string SummaryLine(std::string_view key, std::string_view value) {
  return "summary: " + std::string(key) + ": " + std::string(value);
}

// The last `n` lines of text (all of it when shorter). A trailing partial line counts as a line.
inline std::string LastLines(std::string_view text, size_t n) {
  size_t pos = text.size();
  if (pos && text[pos - 1] == '\n') --pos;
  size_t lines = 0;
  while (pos > 0) {
    const size_t nl = text.rfind('\n', pos - 1);
    if (nl == std::string_view::npos) return std::string(text);
    if (++lines == n) return std::string(text.substr(nl + 1));
    pos = nl;
  }
  return std::string(text);
}

// Lines of `text` that mention any of `needles` (the GPU lines pulled out of a log for the bundle).
inline std::vector<std::string> LinesContaining(std::string_view text, const std::vector<std::string>& needles,
                                                size_t max_lines = 12) {
  std::vector<std::string> out;
  std::istringstream in{std::string(text)};
  for (std::string line; std::getline(in, line) && out.size() < max_lines;)
    for (const std::string& n : needles)
      if (line.find(n) != std::string::npos) {
        out.push_back(line);
        break;
      }
  return out;
}


// ---- Probes (diagnostics.cpp; the game only) ----

// What the app tells the startup summary.
struct StartInfo {
  std::string version;                 // "1.15.0 (commit abc1234)"
  std::filesystem::path data_root;     // the data folder, empty when the platform default is used
  std::string data_source;             // "BOTS_DATA_DIR", "portable.txt" or "" for the default
  std::filesystem::path program;       // the running executable
};

// Logs the "env:" lines now, then probes the machine on a background thread and logs the first "summary:" lines (OS,
// CPU, memory, how the game was started, install kind, data folder). Returns at once.
void StartStartupSummary(const StartInfo& info, std::function<void(const std::string&)> log);

// What the app knows once the game folder is final and the window exists (called from the main thread, which owns the
// window and SDL); the file system questions about the game folder run on a background thread.
struct GameInfo {
  std::filesystem::path game_dir;
  std::string renderer;                // "new (n1)" or "pre-1.0"
  std::string display;                 // "1280x720 windowed, display scale 100%"
  std::string hdr;                     // "off (setting), display HDR off"
  std::string controllers;             // "1: Xbox 360 (Steam Virtual Gamepad)" or "none connected yet"
};
void AddGameSummary(const GameInfo& info, std::function<void(const std::string&)> log);

// The summary lines gathered so far (each "summary: key: value"), waiting briefly for the startup thread.
std::vector<std::string> SummaryLines();

// Writes <folder>/skullburglar-diagnostics.txt: the summary block, the GPU lines from the newest log, and the end of
// the newest logs (`tail_lines` each, at most two). Returns the file's path, or an error message in `error`.
std::filesystem::path SaveBundle(const std::filesystem::path& log_folder, size_t tail_lines, std::string& error);

}  // namespace bots::diag
