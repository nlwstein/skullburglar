// Plain-assert tests for src/diag.h. Run: c++ -std=c++20 -I src tests/diag_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "diag.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  using namespace bots;
  CHECK(!DiagFlagOn(nullptr) && !DiagFlagOn("") && !DiagFlagOn("0"));
  CHECK(DiagFlagOn("1") && DiagFlagOn("true") && DiagFlagOn("yes"));
  // Off by default: the line's threshold is 0 (no line); on, its normal value.
  CHECK(DiagThreshold(nullptr, false, 25.0) == 0.0 && DiagThreshold("", false, 25.0) == 0.0);
  CHECK(DiagThreshold(nullptr, true, 25.0) == 25.0);
  // An explicit value wins either way, and 0 keeps the line off even with diagnostics on.
  CHECK(DiagThreshold("40", false, 25.0) == 40.0 && DiagThreshold("40", true, 25.0) == 40.0);
  CHECK(DiagThreshold("0", true, 25.0) == 0.0);
  if (failures) return 1;
  std::puts("diag_test: ok");
  return 0;
}
