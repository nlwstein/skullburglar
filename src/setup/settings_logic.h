// Pure rules for how the launcher's settings meet bots.toml, kept free of the SDK so tests/settings_reset_test.cpp can
// build them with only toml++ (see that file).
#pragma once

#include <cmath>
#include <cstdlib>
#include <string>

#include <toml++/toml.hpp>

namespace bots::setup {

// Setting values compare as text, or as numbers when both read as one ("1.000000" is "1").
inline bool SameValue(const std::string& a, const std::string& b) {
  if (a == b) return true;
  if (a.empty() || b.empty()) return false;
  char* end_a = nullptr;
  char* end_b = nullptr;
  const double x = std::strtod(a.c_str(), &end_a);
  const double y = std::strtod(b.c_str(), &end_b);
  return *end_a == 0 && *end_b == 0 && std::abs(x - y) < 1e-6;
}

// Saving writes a key only when it says something: the player's value differs from the version's default, or the key
// is already in the file. A key that was reset (removed) and still has the default value stays out, so a later
// version's new default reaches that player.
inline bool ShouldWriteKey(const toml::table& table, const std::string& key, const std::string& value,
                           const std::string& default_value) {
  return table.contains(key) || !SameValue(value, default_value);
}

// Settings kept when everything else goes back to the defaults: the player's update preference. (The game folder and
// data paths are not settings in the list at all; they have their own controls.)
inline bool ResetKeeps(const std::string& cvar) { return cvar == "check_updates"; }

}  // namespace bots::setup
