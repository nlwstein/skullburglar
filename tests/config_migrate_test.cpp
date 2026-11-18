// Old defaults saved in bots.toml are dropped so a new default applies (B-360). Needs the SDK's toml++ headers:
//   c++ -std=c++20 -I src/setup -I <rexglue-sdk>/thirdparty/tomlplusplus/include tests/config_migrate_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "config_migrate.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace bots::setup;

static int Run(const char* text, toml::table& t) {
  t = toml::parse(text);
  return DropOldDefaults(t, kOldDefaults, std::size(kOldDefaults));
}

int main() {
  toml::table t;
  CHECK(Run("kbm_left_trigger = \"RMB\"\nfullscreen = true", t) == 1);
  CHECK(!t.contains("kbm_left_trigger") && t.contains("fullscreen"));
  CHECK(Run("kbm_left_trigger = \" rmb \"", t) == 1);               // trimmed, any case
  CHECK(Run("kbm_left_trigger = \"RMB,Shift\"", t) == 0);           // the new default is kept
  CHECK(Run("kbm_left_trigger = \"LMB\"", t) == 0);                 // a deliberate change is kept
  CHECK(Run("kbm_left_trigger = \"RMB,Z\"", t) == 0);               // so is an added key
  CHECK(Run("kbm_left_trigger = \"\"", t) == 0);
  CHECK(Run("kbm_left_trigger = 5", t) == 0);                       // not a string: untouched
  CHECK(Run("fullscreen = true", t) == 0);
  // The table is general: another row works the same way.
  const OldDefault rows[] = {{"kbm_sensitivity", "1.0"}, {"kbm_a", "Space"}};
  t = toml::parse("kbm_a = \"space\"\nkbm_sensitivity = \"1.0\"\nvibration = false");
  CHECK(DropOldDefaults(t, rows, 2) == 2 && t.size() == 1 && t.contains("vibration"));
  std::printf(failures ? "%d failure(s)\n" : "ok\n", failures);
  return failures != 0;
}
