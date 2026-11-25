// Drift-gate simulation (B-269): synthetic gyro-as-mouse motion through the 1.4.0 gate and the drift gate that replaced
// it, with the same code the game runs (src/input_math.h, src/mouse_pattern.h).
//   c++ -std=c++20 -I src tests/mouse_gate_sim.cpp -o /tmp/sim && /tmp/sim            tables (markdown)
//   /tmp/sim --check                                                                   assertions only (CI)
//   /tmp/sim --tau 0.5 --close 0.5 --secs 60                                           try other values
// One count is 0.33 degrees at 1093 counts per 360 degrees (the reporter's Steam Input setting), 60 frames a second.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "input_math.h"
#include "mouse_pattern.h"

using namespace bots::input;

namespace {
double g_tau = MouseGate::kTau, g_close = MouseGate::kClose, g_secs = 60;
constexpr double kDt = 1.0 / 60;
constexpr double kDegPerCount = 360.0 / 1093;

struct Result {
  double in = 0, out = 0;     // sum of |x| counts in and out (x axis; y only matters for the "y" patterns)
  int frames_out = 0;         // frames that moved the camera
  double max_step = 0;        // biggest single-frame output, counts
  double first_out_ms = -1;   // time of the first output
  double net_in = 0, net_out = 0;  // signed sums (zero-mean jitter cancels): how far the camera really turned
  double mean_lag = 0, max_lag = 0;  // how far the camera is behind the input, counts
};

enum class Algo { kLegacy, kDrift, kStrict };

Result Run(const std::string& spec, Algo algo, double gate, double secs, uint32_t seed = 7) {
  MousePattern pat;
  if (!pat.Parse(spec, seed)) {
    std::fprintf(stderr, "bad spec %s\n", spec.c_str());
    std::exit(2);
  }
  MouseGate g;
  g.close = g_close;
  double acc_x = 0, acc_y = 0, cum_in = 0, cum_out = 0, lag_sum = 0;
  Result r;
  const int n = int(secs / kDt + 0.5);
  for (int i = 0; i < n; ++i) {
    const auto c = pat.Next(kDt);
    double ox, oy;
    if (algo == Algo::kLegacy) {
      acc_x += c.x, acc_y += c.y;
      if (MouseGateHolds(acc_x, acc_y, gate)) ox = oy = 0;
      else ox = acc_x, oy = acc_y, acc_x = acc_y = 0;
    } else {
      const auto o = g.Step(c.x, c.y, kDt, gate, g_tau, algo == Algo::kStrict);
      ox = o.x, oy = o.y;
    }
    // x axis for the camera metrics (the y patterns are only used to see that both axes are gated together)
    r.in += std::fabs(c.x), r.out += std::fabs(ox);
    cum_in += c.x, cum_out += ox;
    r.net_in = cum_in, r.net_out = cum_out;
    if (ox != 0 || oy != 0) {
      ++r.frames_out;
      r.max_step = std::max(r.max_step, std::hypot(ox, oy));
      if (r.first_out_ms < 0) r.first_out_ms = (i + 1) * kDt * 1000;
    }
    const double lag = std::fabs(cum_in - cum_out);
    lag_sum += lag, r.max_lag = std::max(r.max_lag, lag);
  }
  r.mean_lag = lag_sum / n;
  return r;
}

int g_failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_failures; } } while (0)

struct Case {
  const char* name;
  const char* spec;
};
}  // namespace

int main(int argc, char** argv) {
  bool check = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--check")) check = true;
    else if (!std::strcmp(argv[i], "--tau") && i + 1 < argc) g_tau = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--close") && i + 1 < argc) g_close = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--secs") && i + 1 < argc) g_secs = std::atof(argv[++i]);
  }

  static const Case kDrift[] = {
      {"steady 0.5/s", "steady:0.5"}, {"steady 1/s", "steady:1"},     {"steady 2/s", "steady:2"},
      {"steady 3/s", "steady:3"},     {"steady 4/s", "steady:4"},     {"steady 6/s", "steady:6"},
      {"poisson 1/s", "poisson:1"},   {"poisson 3/s", "poisson:3"},   {"poisson 6/s", "poisson:6"},
      {"poisson 3/s, 80% one way", "poisson:3:0.8"}, {"alternating +1/-1 at 30/s", "alt:30"},
      {"random jitter p=0.1", "noise:0.1"}, {"random jitter p=0.3", "noise:0.3"},
      {"random jitter p=0.3, 70% one way", "noise:0.3:0.7"},
  };
  static const Case kAim[] = {
      {"slow turn 5/s (1.6 deg/s)", "steady:5"},   {"slow turn 10/s (3.3 deg/s)", "steady:10"},
      {"slow turn 20/s (6.6 deg/s)", "steady:20"}, {"turn 40/s (13 deg/s)", "steady:40"},
      {"turn 80/s (26 deg/s)", "steady:80"},       {"fast turn 240/s (79 deg/s)", "steady:240"},
      {"slow turn 20/s + poisson drift 3/s", "steady:20+poisson:3:0.8"},
  };
  static const Case kEdge[] = {
      {"rare +1/-1 (Deck-like), p=0.02", "noise:0.02"}, {"+1/-1, p=0.15", "noise:0.15"}, {"+1/-1, p=0.3", "noise:0.3"},
      {"1-2 counts, p=0.15", "burst:0.15:2"}, {"1-3 counts, p=0.3", "burst:0.3:3"},
      {"1-3 counts, p=0.3, + 2/s drift 90% one way", "burst:0.3:3+poisson:2:0.9"},
  };
  static const Case kEdgeAim[] = {
      {"slow turn 5/s (1.6 deg/s)", "steady:5"}, {"slow turn 10/s", "steady:10"}, {"turn 20/s", "steady:20"},
      {"flick 240/s", "steady:240"}, {"turn 20/s + 1-2 count jitter", "steady:20+burst:0.15:2"},
      {"turn 10/s + +1/-1 jitter p=0.15", "steady:10+noise:0.15"},
  };
  static const double kGates[] = {1, 2, 3, 4, 8};

  if (!check) {
    std::printf("tau %.2f s, close %.2f, %.0f s of drift, 3 s of aiming, %.0f fps, %.3f deg per count\n\n", g_tau, g_close,
                g_secs, 1 / kDt, kDegPerCount);
    std::printf("## Drift and noise, %.0f s: share of the counts that reach the camera, and camera steps\n\n", g_secs);
    std::printf("Gate pixels |");
    for (double gate : kGates) std::printf(" %g old: leak, steps, max | %g new |", gate, gate);
    std::printf("\n");
    std::printf("---|");
    for (size_t i = 0; i < 2 * (sizeof kGates / sizeof *kGates); ++i) std::printf("---|");
    std::printf("\n");
    for (const auto& c : kDrift) {
      std::printf("%s |", c.name);
      for (double gate : kGates) {
        for (Algo a : {Algo::kLegacy, Algo::kDrift}) {
          const Result r = Run(c.spec, a, gate, g_secs);
          std::printf(" %.0f%% / %d / %.0f |", r.in > 0 ? 100 * r.out / r.in : 0, r.frames_out, r.max_step);
        }
      }
      std::printf("\n");
    }
    std::printf("\n## Deliberate turns, 3 s: share passed / first output / mean lag in counts / biggest single step in counts -- gate pixels\n\n");
    std::printf("Motion |");
    for (double gate : kGates) std::printf(" %g old | %g new |", gate, gate);
    std::printf("\n---|");
    for (size_t i = 0; i < 2 * (sizeof kGates / sizeof *kGates); ++i) std::printf("---|");
    std::printf("\n");
    for (const auto& c : kAim) {
      std::printf("%s |", c.name);
      for (double gate : kGates) {
        for (Algo a : {Algo::kLegacy, Algo::kDrift}) {
          const Result r = Run(c.spec, a, gate, 3);
          std::printf(" %.0f%% / %.0f ms / %.1f / %.0f |", r.in > 0 ? 100 * r.out / r.in : 0, r.first_out_ms, r.mean_lag,
                      r.max_step);
        }
      }
      std::printf("\n");
    }
    std::printf("\n## Wide jitter (an Edge-class gyro), %.0f s, gate 1.6: counts a second reaching the camera, normal gate -> strict gate\n\n", g_secs);
    std::printf("Jitter | normal | strict |\n---|---|---|\n");
    for (const auto& c : kEdge) {
      const Result a = Run(c.spec, Algo::kDrift, 1.6, g_secs), b = Run(c.spec, Algo::kStrict, 1.6, g_secs);
      std::printf("%s | %.2f | %.2f |\n", c.name, a.out / g_secs, b.out / g_secs);
    }
    std::printf("\n## Slow aim, 3 s, gate 1.6: share of the turn that reaches the camera (signed), normal -> strict\n\n");
    std::printf("Motion | normal | strict |\n---|---|---|\n");
    for (const auto& c : kEdgeAim) {
      const Result a = Run(c.spec, Algo::kDrift, 1.6, 3), b = Run(c.spec, Algo::kStrict, 1.6, 3);
      std::printf("%s | %.1f%% | %.1f%% |\n", c.name, 100 * a.net_out / a.net_in, 100 * b.net_out / b.net_in);
    }
    return 0;
  }

  // Assertions (CI). The numbers are the design's promises, from the tables above.
  // 0. The pattern generator: counts per second as asked, +1/-1 cancelling, sums, bad specs refused.
  {
    auto total = [](const char* spec, double secs, double* ysum = nullptr) {
      MousePattern p;
      CHECK(p.Parse(spec, 3));
      double sx = 0, sy = 0;
      for (int i = 0; i < int(secs / kDt + 0.5); ++i) {
        const auto c = p.Next(kDt);
        sx += c.x, sy += c.y;
      }
      if (ysum) *ysum = sy;
      return sx;
    };
    double y = 0;
    CHECK(total("steady:10", 3) == 30);
    CHECK(total("steady:-10", 3) == -30);
    CHECK(total("steady:6:y", 2, &y) == 0 && y == 12);
    CHECK(std::fabs(total("alt:30", 4)) <= 1);
    CHECK(total("steady:10+steady:5", 2) == 30);
    CHECK(std::fabs(total("poisson:20", 20) - 400) < 60);
    MousePattern bad;
    CHECK(!bad.Parse("") && !bad.Parse("nonsense:3") && !bad.Parse("steady") && !bad.Parse("steady:1+"));
  }
  // 1. Gate 0 is a plain pass-through.
  for (const char* spec : {"steady:1", "noise:0.3", "steady:20+poisson:3"}) {
    const Result r = Run(spec, Algo::kDrift, 0, 5);
    CHECK(r.out == r.in);
  }
  // 2. A sustained slow drift, evenly spaced, never reaches the camera: at gate 2 up to 3 counts a second, at gate 4 up
  //    to 6. The 1.4.0 gate lets nearly all of it through, in steps of the gate size.
  for (const char* spec : {"steady:0.5", "steady:1", "steady:2", "steady:3"}) {
    const Result drift = Run(spec, Algo::kDrift, 2, 60), old = Run(spec, Algo::kLegacy, 2, 60);
    CHECK(drift.out == 0);
    CHECK(old.out > 0.9 * old.in);  // what the reporter saw: held back only until it adds up
  }
  for (const char* spec : {"steady:4", "steady:6"}) CHECK(Run(spec, Algo::kDrift, 4, 60).out == 0);
  // Drift that arrives at random clumps now and then; a bigger gate takes the clumps too.
  CHECK(Run("poisson:1", Algo::kDrift, 2, 60).out <= 0.2 * Run("poisson:1", Algo::kDrift, 2, 60).in);
  CHECK(Run("poisson:1", Algo::kDrift, 3, 60).out == 0);
  CHECK(Run("poisson:3:0.8", Algo::kDrift, 4, 60).out <= 0.1 * Run("poisson:3:0.8", Algo::kDrift, 4, 60).in);
  // 3. Noise around zero: +1/-1 is eaten at gate 2, light random jitter at gate 4.
  CHECK(Run("alt:30", Algo::kDrift, 2, 60).out == 0);
  CHECK(Run("noise:0.1", Algo::kDrift, 4, 60).out == 0);
  // 4. A deliberate turn passes (all but the counts that open the gate), within 0.4 s, at 10 counts a second and up.
  for (const char* spec : {"steady:10", "steady:20", "steady:80", "steady:240"}) {
    const Result r = Run(spec, Algo::kDrift, 2, 3);
    CHECK(r.out >= r.in - 3);
    CHECK(r.first_out_ms <= 400);
  }
  // 5. A deliberate turn with a drift riding on it passes.
  {
    const Result r = Run("steady:20+poisson:3:0.8", Algo::kDrift, 2, 3);
    CHECK(r.out >= r.in - 4);
  }
  // 6. The gate closes again after a turn, and a drift afterwards is dropped.
  {
    MouseGate g;
    double out = 0;
    for (int i = 0; i < 60; ++i) out += g.Step(i % 3 == 0 ? 1 : 0, 0, kDt, 2).x;  // 20 counts/s for 1 s
    CHECK(out >= 18);
    double after = 0;
    MousePattern drift;
    drift.Parse("steady:1");
    for (int i = 0; i < 60 * 20; ++i) after += g.Step(drift.Next(kDt).x, 0, kDt, 2).x;
    CHECK(after <= 2);  // at most what leaks through while the gate is still open after the turn
  }
  // 7. Both axes count together: a diagonal drift is as quiet as a straight one, a diagonal turn passes.
  {
    MouseGate g;
    double out = 0;
    for (int i = 0; i < 60 * 30; ++i) {
      const auto o = g.Step(i % 90 == 0 ? 1 : 0, i % 90 == 45 ? 1 : 0, kDt, 2);  // 0.67/s x and y
      out += std::fabs(o.x) + std::fabs(o.y);
    }
    CHECK(out == 0);
  }
  // 8. Strict mode (B-396). Off, the gate is exactly the normal one. On, steady motion is unchanged (a coherent sum equals
  //    its path), wide random jitter that leaks through the normal gate is cut at least 4x (2x for the mildest, 1-2 counts at p=0.15), and a turn still passes.
  for (const char* spec : {"steady:1", "steady:5", "steady:20", "steady:240", "noise:0.02", "poisson:1:0.8", "poisson:3:0.8",
                           "steady:20+poisson:3:0.8"}) {
    const Result a = Run(spec, Algo::kDrift, 1.6, 20), b = Run(spec, Algo::kStrict, 1.6, 20);
    CHECK(a.out == b.out && a.frames_out == b.frames_out);
  }
  for (const char* spec : {"noise:0.3", "burst:0.15:2", "burst:0.3:3", "burst:0.3:3+poisson:2:0.9"}) {
    const int factor = std::strcmp(spec, "burst:0.15:2") == 0 ? 2 : 4;  // the mildest jitter is the hardest to tell from a turn
    double a = 0, b = 0;
    for (uint32_t seed : {3u, 7u, 11u, 13u}) a += Run(spec, Algo::kDrift, 1.6, 120, seed).out, b += Run(spec, Algo::kStrict, 1.6, 120, seed).out;
    CHECK(b * factor <= a);
  }
  for (const char* spec : {"steady:5", "steady:10", "steady:20", "steady:240", "steady:20+burst:0.15:2"}) {
    const Result r = Run(spec, Algo::kStrict, 1.6, 3);
    CHECK(r.net_out >= 0.93 * r.net_in);
    CHECK(r.first_out_ms <= 700);
  }
  // 9. Opening releases a capped backlog (#21 second report): jitter that finally opens the gate moves the camera by
  //    about a gate, not by the whole held-back sum (1.15.0: up to 6-8 counts in one frame).
  for (const char* spec : {"noise:0.3", "burst:0.15:2", "burst:0.3:3", "poisson:6:0.8"}) {
    for (uint32_t seed : {3u, 7u, 11u}) {
      const Result r = Run(spec, Algo::kStrict, 1.6, 120, seed);
      CHECK(r.max_step <= 1.6 * MouseGate{}.backlog_cap + 3.01);  // the cap plus the biggest frame of counts (3)
    }
  }
  {  // a flick still passes whole (its own frame's counts are never capped)
    MouseGate g;
    const auto o = g.Step(12, 0, kDt, 1.6);
    CHECK(o.x == 12);
  }
  {  // strict off must not even track the coherence state
    MouseGate g;
    for (int i = 0; i < 100; ++i) g.Step(1, 0, kDt, 1.6);
    CHECK(g.path == 0);
  }
  if (g_failures) return 1;
  std::puts("mouse_gate_sim: all passed");
  return 0;
}
