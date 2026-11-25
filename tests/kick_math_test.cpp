// Plain-assert tests for src/kick_math.h. Run: c++ -std=c++20 -I src tests/kick_math_test.cpp -o /tmp/t && /tmp/t
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

#include "kick_math.h"

int main() {
  using namespace bots::kick;
  assert(Factor(100.0) == 1.0f && Factor(250.0) == 1.0f);
  assert(Factor(0.0) == 0.0f && Factor(-5.0) == 0.0f);
  assert(std::abs(Factor(25.0) - 0.25f) < 1e-6f);
  assert(Factor(std::numeric_limits<double>::quiet_NaN()) == 1.0f);

  assert(Scaled(1.5f, 1.0f) == 1.5f);          // default: bit-exact
  assert(Scaled(1.5f, 0.0f) == 0.0f);
  assert(std::abs(Scaled(-2.0f, 0.5f) + 1.0f) < 1e-6f);

  assert(PlausibleCurve({0.0f, 0.5f, 10.0f}));
  assert(PlausibleCurve({3.0f}));
  assert(!PlausibleCurve({}));
  assert(!PlausibleCurve({0.0f, 2.0f, 1.0f}));                                   // InVal went backwards
  assert(!PlausibleCurve({0.0f, std::numeric_limits<float>::infinity()}));
  assert(!PlausibleCurve({0.0f, std::numeric_limits<float>::quiet_NaN()}));
  assert(!PlausibleCurve(std::vector<float>(kMaxPoints + 1, 1.0f)));             // a count that large is the wrong layout
  assert(PlausibleCurve(std::vector<float>(kMaxPoints, 1.0f)));
  assert(kPointSize == 4 + 3 * 12 + 4);
  assert(kFloatsPerPoint * 4 + kFirstScaledOffset + 4 == kPointSize);
  return 0;
}
