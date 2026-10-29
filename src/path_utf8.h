// Paths and UTF-8 text. On Windows, std::filesystem::path::string() converts to the ANSI code page and throws
// (ERROR_NO_UNICODE_TRANSLATION) for a name that code page can't hold, such as a Russian "Documents" folder on a
// Western-European system. The game's strings (settings, logs, the UI, command-line values) are UTF-8, so a path
// goes in and out of them through these two helpers, never through string() or the narrow path(std::string)
// constructor. On macOS and Linux both are plain copies.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

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

}  // namespace bots
