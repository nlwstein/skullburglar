// Plain-assert tests for src/setup/occlusion_choice.h (B-333). Run: c++ -std=c++20 -I src tests/occlusion_choice_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstring>

#include "setup/occlusion_choice.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  using bots::setup::Pre1OcclusionEnv;
  CHECK(Pre1OcclusionEnv("off") && std::strcmp(Pre1OcclusionEnv("off"), "0") == 0);
  CHECK(Pre1OcclusionEnv("on") && std::strcmp(Pre1OcclusionEnv("on"), "1") == 0);
  CHECK(Pre1OcclusionEnv("auto") == nullptr);
  CHECK(Pre1OcclusionEnv("") == nullptr);
  CHECK(Pre1OcclusionEnv("Off") == nullptr);  // exact words only; a typo is automatic, never a silent "off"
  CHECK(Pre1OcclusionEnv("0") == nullptr);
  if (failures) return 1;
  std::printf("occlusion_choice_test: ok\n");
  return 0;
}
