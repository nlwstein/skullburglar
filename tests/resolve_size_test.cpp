// Plain-assert tests for src/gfx/vk/resolve_size.h. Run: c++ -std=c++20 -I src tests/resolve_size_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "gfx/vk/resolve_size.h"

using namespace bots::gfx::vk;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  // Which resolves stay at guest size: the blur chain's 352x182 does, the full frame, the half-resolution buffers and
  // the tiny tracking textures do not.
  CHECK(SmallResolveSize(352, 182));
  CHECK(!SmallResolveSize(1280, 720));
  CHECK(!SmallResolveSize(640, 360));
  CHECK(!SmallResolveSize(128, 64));
  CHECK(!SmallResolveSize(128, 128));
  CHECK(!SmallResolveSize(864, 864));
  CHECK(SmallResolveSize(256, 128));
  if (failures) return 1;
  std::printf("resolve_size_test: ok\n");
  return 0;
}
