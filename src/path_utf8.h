// Paths and UTF-8 text. On Windows, std::filesystem::path::string() converts to the ANSI code page and throws
// (ERROR_NO_UNICODE_TRANSLATION) for a name that code page can't hold, such as a Russian "Documents" folder on a
// Western-European system. The game's strings (settings, logs, the UI, command-line values) are UTF-8, so a path
// goes in and out of them through these two helpers, never through string() or the narrow path(std::string)
// constructor. On macOS and Linux both are plain copies.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace bots {

// The path as UTF-8 text, for logs, the UI, comparisons, settings and arguments passed on to other programs.
inline std::string PathUtf8(const std::filesystem::path& p) {
  const auto u = p.u8string();  // std::u8string, or std::string when built with -fno-char8_t
  return std::string(u.begin(), u.end());
}

// The path for UTF-8 text (a settings value, a command-line argument, a text box).
inline std::filesystem::path PathFromUtf8(std::string_view s) {
#if defined(__cpp_char8_t)
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
  std::string copy(s);
  // Without char8_t the string constructor means "ANSI" on Windows, so go through u8path.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  return std::filesystem::u8path(copy);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
}

// fopen() for a path. Windows opens by the wide name (fopen() takes the ANSI code page, which can't hold every name).
inline std::FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#if defined(_WIN32)
  std::wstring wmode;
  for (const char* c = mode; *c; ++c) wmode.push_back(wchar_t(static_cast<unsigned char>(*c)));
  return _wfopen(p.c_str(), wmode.c_str());
#else
  return std::fopen(p.c_str(), mode);
#endif
}

// An environment variable holding a path (LOCALAPPDATA, HOME ...); empty when unset. Windows reads the wide
// variable: getenv() would turn a Cyrillic user name into question marks.
inline std::filesystem::path EnvPath(const char* name) {
#if defined(_WIN32)
  std::wstring wname;
  for (const char* c = name; *c; ++c) wname.push_back(wchar_t(static_cast<unsigned char>(*c)));
  if (const wchar_t* value = _wgetenv(wname.c_str()); value && *value) return std::filesystem::path(value);
  return {};
#else
  if (const char* value = std::getenv(name); value && *value) return std::filesystem::path(value);
  return {};
#endif
}

// Whether the text is well-formed UTF-8 (no overlong forms, surrogates or values past U+10FFFF). A Linux folder name
// can be any bytes, so a path is not always valid UTF-8 text: toml++ rewrites such text when it saves it and refuses
// it when it reads it, so settings store those paths as hex (SettingFromPath).
inline bool IsValidUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    uint32_t cp = 0, min = 0;
    if (c < 0x80) { ++i; continue; }
    if (c >= 0xC2 && c <= 0xDF) n = 1, cp = c & 0x1F, min = 0x80;
    else if (c >= 0xE0 && c <= 0xEF) n = 2, cp = c & 0x0F, min = 0x800;
    else if (c >= 0xF0 && c <= 0xF4) n = 3, cp = c & 0x07, min = 0x10000;
    else return false;
    if (i + n >= s.size()) return false;  // cut off in the middle of a character
    for (size_t k = 1; k <= n; ++k) {
      const unsigned char d = static_cast<unsigned char>(s[i + k]);
      if ((d & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += n + 1;
  }
  return true;
}

// Hex text for raw bytes, and back (empty, with ok = false, when the text isn't hex).
inline std::string HexEncode(std::string_view bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (const char c : bytes) {
    out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 15]);
    out.push_back(digits[static_cast<unsigned char>(c) & 15]);
  }
  return out;
}
inline std::string HexDecode(std::string_view hex, bool* ok = nullptr) {
  std::string out;
  auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  bool good = hex.size() % 2 == 0;
  for (size_t i = 0; good && i < hex.size(); i += 2) {
    const int a = nibble(hex[i]), b = nibble(hex[i + 1]);
    if (a < 0 || b < 0) good = false;
    else out.push_back(static_cast<char>(a * 16 + b));
  }
  if (ok) *ok = good;
  return good ? out : std::string();
}

// The same folder, whichever way it was spelled. Plain comparison of two paths compares their bytes, and macOS gives
// the same name as NFC (a single "e with acute") or NFD ("e" plus a combining accent) depending on who made it: the
// home folder variable, the folder picker and Finder disagree. Two paths that exist are compared by the file system
// (same file), so a folder reached both ways is one folder.
inline bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
  if (a == b) return true;
  std::error_code ec;
  return std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec) && std::filesystem::equivalent(a, b, ec) && !ec;
}

// A file:// URL for a path, for SDL_OpenURL: every byte outside A-Z a-z 0-9 - . _ ~ / : is %-escaped, so spaces, "%",
// "#", "?" and non-ASCII names (which macOS's URL parser rejects) all survive.
inline std::string FileUrl(const std::string& utf8_path) {
  std::string path = utf8_path;
  if (path.empty() || path.front() != '/') path.insert(path.begin(), '/');  // C:/x -> /C:/x
  std::string out = "file://";
  static const char* digits = "0123456789ABCDEF";
  for (const char ch : path) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
        c == '~' || c == '/' || c == ':') {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(digits[c >> 4]);
      out.push_back(digits[c & 15]);
    }
  }
  return out;
}

}  // namespace bots
