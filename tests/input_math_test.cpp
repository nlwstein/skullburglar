// Plain-assert tests for src/input_math.h. Run: c++ -std=c++20 -I src tests/input_math_test.cpp -o /tmp/t && /tmp/t
#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "input_math.h"

using namespace bots::input;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(double a, double b, double e) { return std::fabs(a - b) <= e; }

int main() {
  // The original setting (20%) and anything above it pass the stick through untouched.
  CHECK((RemapRightStick({1234, -5678}, 0.2) == Stick{1234, -5678}));
  CHECK((RemapRightStick({-32768, 100}, 0.25) == Stick{-32768, 100}));

  // Dead zone 0: the game's own dead zone is cancelled, so the game's output equals the stick on every axis.
  for (int v : {1, 100, 1000, 6000, 12000, 20000, 32000}) {
    for (int sign : {1, -1}) {
      const Stick out = RemapRightStick({int16_t(sign * v), 0}, 0.0);
      CHECK(Near(GameAxisDeadZone(out.x), sign * v / 32768.0, 2.0 / 32768));
      CHECK(out.y == 0);
    }
  }
  // Diagonal keeps its direction (no per-axis snapping).
  {
    const Stick out = RemapRightStick({3000, 6000}, 0.0);
    const double gx = GameAxisDeadZone(out.x), gy = GameAxisDeadZone(out.y);
    CHECK(gx > 0 && gy > 0);
    CHECK(Near(gy / gx, 2.0, 0.01));
  }
  // A round dead zone: inside it nothing, just outside it a little, at full travel full.
  CHECK((RemapRightStick({0, 0}, 0.1) == Stick{}));
  CHECK((RemapRightStick({2000, 2000}, 0.1) == Stick{}));  // |v| = 0.086
  {
    const Stick out = RemapRightStick({5000, 0}, 0.1);  // 0.153 of travel
    CHECK(Near(GameAxisDeadZone(out.x), (0.15259 - 0.1) / 0.9, 0.002));
  }
  CHECK((RemapRightStick({32767, 0}, 0.1).x >= 32766));
  CHECK((RemapRightStick({-32768, 0}, 0.0).x <= -32766));
  // Magnitude stays monotonic with the dead zone: a smaller dead zone never gives less.
  CHECK(GameAxisDeadZone(RemapRightStick({9000, 0}, 0.0).x) > GameAxisDeadZone(RemapRightStick({9000, 0}, 0.15).x));

  // Response curve: squared yaw as shipped, linear with TurnPower 1 (table in the plan: 0.375 -> 14%).
  CHECK(Near(YawResponse(0.375, 2.0), 0.1406, 0.001));
  CHECK(Near(YawResponse(-0.375, 2.0), -0.1406, 0.001));
  CHECK(Near(YawResponse(0.375, 1.0), 0.375, 1e-9));
  CHECK(Near(YawResponse(1.0, 2.0), 1.0, 1e-9));

  // The LagAmount trap: 1 means the camera never turns; the shipped 1.1 is 0.147 at 60 fps; "no lag" is ~1.
  CHECK(LagProportion(1.0, 1.0 / 60, 1.0) == 0.0);
  CHECK(Near(LagProportion(1.1, 1.0 / 60, 1.0), 0.147, 0.001));
  CHECK(LagProportion(kNoLagAmount, 1.0 / 60, 1.0) > 0.999999);
  CHECK(LagProportion(kNoLagAmount, 1.0 / 240, 1.0) > 0.99);

  // Mouse noise gate: off by default, holds sub-threshold motion, releases once it adds up, keeps fractions.
  CHECK(!MouseGateHolds(1, 0, 0));
  CHECK(!MouseGateHolds(0.1, 0, 0));
  CHECK(MouseGateHolds(1, 0, 2));
  CHECK(MouseGateHolds(1, 1, 2));
  CHECK(!MouseGateHolds(2, 0, 2));
  CHECK(!MouseGateHolds(0, -3, 2));
  {
    double ax = 0;  // 1 px every frame, gate 3: held for two frames, then the whole 3 px passes
    int released = 0;
    double passed = 0;
    for (int i = 0; i < 9; ++i) {
      ax += 1;
      if (!MouseGateHolds(ax, 0, 3)) { passed += ax; ax = 0; ++released; }
    }
    CHECK(released == 3 && Near(passed, 9, 1e-9));
    ax = 0;  // jitter +1, -1 never passes
    for (int i = 0; i < 100; ++i) { ax += (i % 2) ? -1 : 1; CHECK(MouseGateHolds(ax, 0, 2)); }
  }

  // Stick look speed (B-303): clamped to 1..3, and it scales the whole steady turn (curve x speed + full-tilt bonus).
  CHECK(ClampLookSpeed(1.0) == 1.0 && ClampLookSpeed(2.5) == 2.5 && ClampLookSpeed(3.0) == 3.0);
  CHECK(ClampLookSpeed(0.2) == 1.0 && ClampLookSpeed(9.0) == 3.0 && ClampLookSpeed(-1.0) == 1.0);
  CHECK(ClampLookSpeed(std::nan("")) == 1.0);
  for (double stick : {0.2, 0.5, 1.0}) {
    for (double bonus : {0.0, 1.6}) {
      const double base = SteadyTurn(stick, 2.0, 1.0, 1.5, bonus, 1.0);
      for (double look : {1.0, 2.0, 3.0}) CHECK(Near(SteadyTurn(stick, 2.0, 1.0, 1.5, bonus, look), base * look, 1e-12));
    }
  }
  CHECK(Near(SteadyTurn(1.0, 2.0, 1.0, 1.0, 1.6, 1.0), 2.6, 1e-12));  // the shipped full-tilt 2.6x

  // Rest deadband (#21, B-396): the threshold in degrees, and what the gate it drives does with a gyro lying still
  // (random +-1 counts) against slow real motion.
  CHECK(Near(DegPerCount(1.0, false), 0.3296, 1e-4) && Near(DegPerCount(1.0, true), 0.2060, 1e-4));
  CHECK(Near(DegPerCount(0.1, false), 0.03296, 1e-5));
  CHECK(RestGateCounts(0, 1, false) == 0 && RestGateCounts(-1, 1, false) == 0 && RestGateCounts(std::nan(""), 1, false) == 0);
  CHECK(Near(RestGateCounts(0.5, 1.0, false), 0.5 / 0.3296, 1e-3));     // 1.5 counts at the default sensitivity
  CHECK(Near(RestGateCounts(0.5, 0.1, false), 0.5 / 0.03296, 1e-2));    // 15 counts when each count is a tenth as big
  CHECK(RestGateCounts(0.1, 4.0, false) == 1.0 && RestGateCounts(2.0, 0.05, false) == kMaxRestGateCounts);
  {
    struct Rng { uint32_t s = 12345; double u() { s ^= s << 13, s ^= s >> 17, s ^= s << 5; return s / 4294967296.0; } } rng;
    constexpr double kDt = 1.0 / 60;
    auto run = [&](double deadband_deg, double sens, auto&& feed, double secs) {
      MouseGate g;
      double in = 0, out = 0, net = 0, first_ms = -1;
      for (int i = 0; i < int(secs / kDt); ++i) {
        const double c = feed(i * kDt);
        const auto o = g.Step(c, 0, kDt, RestGateCounts(deadband_deg, sens, false), MouseGate::kTau, true);
        in += std::fabs(c), out += std::fabs(o.x), net += o.x;
        if (first_ms < 0 && o.x != 0) first_ms = i * kDt * 1000;
      }
      return std::array<double, 4>{in, out, net, first_ms};
    };
    // Table noise: random-sign counts, 2 to 100 a second. Under 8% of them reach the view at the default sensitivity
    // with a 1 degree deadband (3 counts), and nearly none when each count is finer (0.25 and 0.1) with 0.5 degrees.
    for (auto [sens, db] : {std::pair{1.0, 1.0}, std::pair{0.25, 0.5}, std::pair{0.1, 0.5}})
      for (double rate : {2.0, 5.0, 20.0, 100.0}) {
        const auto r = run(db, sens, [&](double) { return rng.u() < rate * kDt ? (rng.u() < 0.5 ? 1.0 : -1.0) : 0.0; }, 120);
        CHECK(r[0] > 0 && r[1] <= 0.08 * r[0] + 3);
        if (sens < 1.0) CHECK(r[1] <= 0.01 * r[0] + 1);
      }
    // Perfectly alternating +1/-1 at 30 a second: nothing at all.
    CHECK(run(0.5, 1.0, [](double t) { return int(t * 30 + 1e-9) != int((t - kDt) * 30 + 1e-9) ? ((int(t * 30 + 1e-9) % 2) ? 1.0 : -1.0) : 0.0; }, 60)[1] == 0);
    // Slow real turns pass: 4 and 6.6 degrees a second at the default sensitivity (12 and 20 counts a second, one
    // direction); more than 90% of the counts arrive and the first one inside half a second.
    for (double cps : {12.0, 20.0}) {
      const auto r = run(0.5, 1.0, [&](double t) { return std::floor(cps * t + 1e-9) - std::floor(cps * (t - kDt) + 1e-9); }, 10);
      CHECK(r[1] >= 0.9 * r[0] && r[3] >= 0 && r[3] < 500);
    }
    // The same 4 degrees a second at sensitivity 0.1 (121 counts a second) passes too, and a flick of 6 counts in one
    // frame opens the gate in that frame.
    const auto fine = run(0.5, 0.1, [&](double t) { return std::floor(121 * t + 1e-9) - std::floor(121 * (t - kDt) + 1e-9); }, 10);
    CHECK(fine[1] >= 0.9 * fine[0]);
    MouseGate g;
    const auto o = g.Step(6, 0, kDt, RestGateCounts(0.5, 1.0, false), MouseGate::kTau, true);
    CHECK(o.x == 6);
    // A slow drift under the deadband (0.3 counts a second, one direction) never opens it.
    CHECK(run(0.5, 1.0, [](double t) { return std::floor(0.3 * t + 1e-9) - std::floor(0.3 * (t - kDt) + 1e-9); }, 120)[1] == 0);
  }

  if (failures) return 1;
  std::puts("input_math: all passed");
  return 0;
}
