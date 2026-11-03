// Pure maths for the test range (src/test_range.cpp): where to stand to summon each target, and a damage-log summary.
// No game or SDK types, so tests/range_math_test.cpp can run it alone.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bots::range {

constexpr float kSummonAhead = 72.0f;  // `Summon` places the pawn this far in front of the player...
constexpr float kSummonUp = 15.0f;     // ...and this far up.

struct Spot {
  float x, y, z;
};

// Unreal rotation units: 65536 per turn.
inline float YawRadians(int32_t yaw_units) { return float(yaw_units & 0xFFFF) * 6.28318530718f / 65536.0f; }

// Target `i` of `n` stands `dist` ahead of the origin along `yaw_units`, spread sideways `spacing` apart and centred
// on the line of fire. Returns where the target ends up.
inline Spot TargetPos(Spot origin, int32_t yaw_units, float dist, float spacing, int i, int n) {
  const float a = YawRadians(yaw_units), fx = std::cos(a), fy = std::sin(a);
  const float side = (float(i) - float(n - 1) * 0.5f) * spacing;
  return {origin.x + fx * dist - fy * side, origin.y + fy * dist + fx * side, origin.z};
}

// The spot the player must stand on, facing `yaw_units`, for `Summon` to put the pawn at `target`.
inline Spot SummonStand(Spot target, int32_t yaw_units) {
  const float a = YawRadians(yaw_units);
  return {target.x - std::cos(a) * kSummonAhead, target.y - std::sin(a) * kSummonAhead, target.z - kSummonUp};
}

// "BugItGo x y z pitch yaw roll" for a stand spot (the console command that moves the player).
inline std::string BugItGo(Spot s, int32_t yaw_units) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "BugItGo %.1f %.1f %.1f 0 %d 0", s.x, s.y, s.z, int(yaw_units));
  return buf;
}

// Shots-to-kill bookkeeping for one victim.
struct Kill {
  int hits = 0, head = 0;
  float damage = 0;
  bool dead = false;
};
inline void Note(Kill& k, float damage, bool head, bool killed) {
  if (k.dead) return;
  ++k.hits;
  k.head += head ? 1 : 0;
  k.damage += damage;
  k.dead = killed;
}

}  // namespace bots::range
