// Plain-assert tests for src/focus_flag.h. Run: c++ -std=c++20 -I src tests/focus_flag_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "focus_flag.h"

using namespace bots::focus;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  // ObjectHasFocus on SfActor: bit 0x00200000 of the word at +0x20C.
  constexpr uint32_t kMask = 0x00200000u;
  CHECK(InstFlags(true, 0x00200000u, kMask) == kInstFocus);
  CHECK(InstFlags(true, 0xFFFFFFFFu, kMask) == kInstFocus);
  CHECK(InstFlags(true, 0, kMask) == 0);
  // Neighbouring bools in the same word don't count.
  CHECK(InstFlags(true, 0x00100000u, kMask) == 0);
  CHECK(InstFlags(true, 0x00400000u, kMask) == 0);
  CHECK(InstFlags(true, 0xFFDFFFFFu, kMask) == 0);
  // A property that wasn't found has mask 0: never set, whatever the word holds.
  CHECK(InstFlags(true, 0xFFFFFFFFu, 0) == 0);
  // The switch.
  CHECK(InstFlags(false, 0x00200000u, kMask) == 0);
  CHECK(EnabledBy(nullptr));
  CHECK(EnabledBy(""));
  CHECK(EnabledBy("1"));
  CHECK(EnabledBy("00"));
  CHECK(!EnabledBy("0"));
  static_assert(kInstFocus == 1);
  if (failures) return 1;
  std::printf("focus_flag_test: ok\n");
  return 0;
}
