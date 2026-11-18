// Default changes that reach players who already have the old default saved in bots.toml (B-360). Older launchers wrote
// every setting on Play, so a player who never touched a setting still has the old default stored, and a new default
// would never apply. A stored value that is exactly the old default is dropped, as if unset, so the current default
// applies. Add a row here whenever a default changes; keep the old value forever (an older file can still hold it).
#pragma once

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>

namespace bots::setup {

struct OldDefault {
  const char* key;
  const char* old_value;  // compared trimmed and case-insensitively
};

// "RMB" was the Aim default until the Shift key was added (RMB,Shift).
inline constexpr OldDefault kOldDefaults[] = {
    {"kbm_left_trigger", "RMB"},
};

inline bool SameTextIgnoringCase(std::string_view a, std::string_view b) {
  const auto trim = [](std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
  };
  a = trim(a);
  b = trim(b);
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// Removes every string value that equals its old default. Returns how many.
inline int DropOldDefaults(toml::table& table, const OldDefault* rows, size_t count) {
  int dropped = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto* node = table.get(rows[i].key);
    if (!node) continue;
    const auto value = node->value<std::string>();
    if (value && SameTextIgnoringCase(*value, rows[i].old_value)) {
      table.erase(rows[i].key);
      ++dropped;
    }
  }
  return dropped;
}

// Applies kOldDefaults to the file; rewrites it only when something was dropped. Returns how many were dropped.
int MigrateOldDefaults(const std::filesystem::path& config_path);

}  // namespace bots::setup
