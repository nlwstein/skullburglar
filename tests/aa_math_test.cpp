// Plain-assert tests for src/aa_math.h. Run: c++ -std=c++20 -I src tests/aa_math_test.cpp -o /tmp/t && /tmp/t
#include <cmath>
#include <cstdio>

#include "aa_math.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  using namespace bots::aa;
  CHECK(Mode("off") == 0 && Mode("fxaa") == 1 && Mode("smaa") == 2);
  // Unknown values are off, including a different case or an empty value.
  CHECK(Mode("") == 0 && Mode("FXAA") == 0 && Mode("msaa") == 0 && Mode("2") == 0);
  CHECK(Sharpening(0.0) == 0.0f && Sharpening(1.0) == 1.0f && Sharpening(0.5) == 0.5f);
  CHECK(Sharpening(-1.0) == 0.0f && Sharpening(7.5) == 1.0f && Sharpening(NAN) == 0.0f);
  if (failures == 0) std::printf("aa_math_test: ok\n");
  return failures ? 1 : 0;
}
