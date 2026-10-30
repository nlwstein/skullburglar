// Plain-assert tests for src/outline_flag.h. Run: c++ -std=c++20 -I src tests/outline_flag_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "outline_flag.h"

using namespace bots::outline;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  // bSpecialHighlight on the game's SkeletalMeshComponent: bit 0x40000000 of the word at +0x1CC.
  constexpr uint32_t kMask = 0x40000000u;
  CHECK(BoolSet(0x40000000u, kMask));
  CHECK(BoolSet(0xFFFFFFFFu, kMask));
  CHECK(!BoolSet(0, kMask));
  // Neighbouring bools in the same word don't count.
  CHECK(!BoolSet(0x80000000u, kMask));
  CHECK(!BoolSet(0x20000000u, kMask));
  CHECK(!BoolSet(0xBFFFFFFFu, kMask));
  CHECK(BoolSet(0x00000001u, 1u));
  // A property that wasn't found has mask 0: never set, whatever the word holds.
  CHECK(!BoolSet(0xFFFFFFFFu, 0));
  // The instance flag.
  CHECK(SkelFlags(0x40000000u, kMask) == kSkelOutline);
  CHECK(SkelFlags(0x3FFFFFFFu, kMask) == 0);
  CHECK(SkelFlags(0xFFFFFFFFu, 0) == 0);
  static_assert(kSkelOutline == 1);
  if (failures) return 1;
  std::printf("outline_flag_test: ok\n");
  return 0;
}
