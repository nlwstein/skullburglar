// The flag rule behind turning off Sticky / Toggle / Filter Keys shortcuts (B-360). Plain C++, any platform:
//   c++ -std=c++20 -I src tests/win_accessibility_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "win_accessibility.h"

using namespace bots::access;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  // Windows default: shortcut armed with a confirm prompt (SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY | SKF_AVAILABLE ...).
  const uint32_t defaults = 0x4 | 0x8 | 0x2 | 0x10 | 0x20;
  const uint32_t blocked = BlockedFlags(defaults);
  CHECK((blocked & kHotkeyActive) == 0 && (blocked & kConfirmHotkey) == 0);
  CHECK((blocked & ~(kHotkeyActive | kConfirmHotkey)) == (defaults & ~(kHotkeyActive | kConfirmHotkey)));  // nothing else
  CHECK(BlockedFlags(blocked) == blocked);                                                  // idempotent
  // The feature itself is on (the player uses Sticky Keys): left exactly as it is.
  const uint32_t on = kFeatureOn | kHotkeyActive | kConfirmHotkey | 0x2;
  CHECK(BlockedFlags(on) == on);
  CHECK(BlockedFlags(0) == 0);
  std::printf(failures ? "%d failure(s)\n" : "ok\n", failures);
  return failures != 0;
}
