// The launcher's settings-file rules (B-359): reset removes keys, saving doesn't bring a default back. Needs the SDK's
// toml++ headers, so it is not in the CI `checks` job:
//   c++ -std=c++20 -I src/setup -I <rexglue-sdk>/thirdparty/tomlplusplus/include tests/settings_reset_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <sstream>

#include "settings_logic.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using bots::setup::ResetKeeps;
using bots::setup::SameValue;
using bots::setup::ShouldWriteKey;

int main() {
  CHECK(SameValue("1", "1.000000"));
  CHECK(SameValue("100.00", "100"));
  CHECK(SameValue("true", "true"));
  CHECK(!SameValue("true", "false"));
  CHECK(!SameValue("60", "30"));
  CHECK(!SameValue("", "0"));
  CHECK(!SameValue("RMB", "RMB,Z"));
  CHECK(SameValue("RMB,Z", "RMB,Z"));

  const auto empty = toml::table{};
  const toml::table has = toml::parse(R"(kbm_sensitivity = 1.0
fullscreen = true)");
  // Default value, key absent (after a reset): stays out of the file.
  CHECK(!ShouldWriteKey(empty, "kbm_sensitivity", "1.00", "1.000000"));
  // Changed value: written. Key already in the file: kept up to date even at the default.
  CHECK(ShouldWriteKey(empty, "kbm_sensitivity", "2.00", "1.000000"));
  CHECK(ShouldWriteKey(has, "kbm_sensitivity", "1.00", "1.000000"));
  CHECK(!ShouldWriteKey(has, "vibration", "true", "true"));

  // Reset = erase the keys, keep the rest (check_updates, folders).
  toml::table t = toml::parse(R"(game_data_root = "/g"
check_updates = false
fullscreen = true
kbm_left_trigger = "RMB")");
  for (const char* k : {"check_updates", "fullscreen", "kbm_left_trigger"})
    if (!ResetKeeps(k)) t.erase(k);
  CHECK(t.contains("game_data_root") && t.contains("check_updates"));
  CHECK(!t.contains("fullscreen") && !t.contains("kbm_left_trigger"));

  std::printf(failures ? "%d failure(s)\n" : "ok\n", failures);
  return failures != 0;
}
