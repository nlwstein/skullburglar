// Plain-assert tests for src/live_mic.h. Run: c++ -std=c++20 -I src tests/live_mic_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "live_mic.h"

using namespace bots::livemic;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  // Switches: BOTS_N1_FOCUS=0 is off whatever else is set; BOTS_N1_LIVE_PARAMS picks the set.
  CHECK(ModeFrom(nullptr, nullptr, Mode::kAll) == Mode::kAll);
  CHECK(ModeFrom("1", nullptr, Mode::kActivated) == Mode::kActivated);
  CHECK(ModeFrom("0", "all", Mode::kAll) == Mode::kOff);
  CHECK(ModeFrom("0", nullptr, Mode::kAll) == Mode::kOff);
  CHECK(ModeFrom("00", nullptr, Mode::kAll) == Mode::kAll);  // only exactly "0" switches it off
  CHECK(ModeFrom(nullptr, "activated", Mode::kAll) == Mode::kActivated);
  CHECK(ModeFrom(nullptr, "all", Mode::kActivated) == Mode::kAll);
  CHECK(ModeFrom(nullptr, "off", Mode::kAll) == Mode::kOff);
  CHECK(ModeFrom(nullptr, "bogus", Mode::kActivated) == Mode::kActivated);

  CHECK(SameName("Activated", "activated"));
  CHECK(!SameName("Activated", "Activate"));
  CHECK(Watched(Mode::kActivated, "ACTIVATED"));
  CHECK(!Watched(Mode::kActivated, "Brightness"));
  CHECK(Watched(Mode::kAll, "Brightness"));
  CHECK(!Watched(Mode::kOff, "Activated"));

  // A poster's instance: Activated cooked at 0, then focused (1), then released.
  const std::vector<Param> first = {{"Activated", 0.f}, {"Glow", 2.f}};
  CHECK(Suffix(Mode::kActivated, first, {{"Activated", 0.f}, {"Glow", 2.f}}).empty());
  CHECK(Suffix(Mode::kActivated, first, {{"Activated", 1.f}, {"Glow", 2.f}}) == "&s:Activated=1");
  CHECK(Suffix(Mode::kActivated, first, {{"Activated", 0.f}, {"Glow", 2.f}}).empty());  // released: the cooked look
  // Other parameters follow only in "all" mode.
  CHECK(Suffix(Mode::kActivated, first, {{"Activated", 0.f}, {"Glow", 5.f}}).empty());
  CHECK(Suffix(Mode::kAll, first, {{"Activated", 0.f}, {"Glow", 5.f}}) == "&s:Glow=5");
  CHECK(Suffix(Mode::kAll, first, {{"Activated", 1.f}, {"Glow", 5.f}}) == "&s:Activated=1&s:Glow=5");
  CHECK(Suffix(Mode::kOff, first, {{"Activated", 1.f}}).empty());
  // A parameter the engine adds on first use counts as changed, even at 0.
  CHECK(Suffix(Mode::kActivated, {}, {{"Activated", 1.f}}) == "&s:Activated=1");
  CHECK(Suffix(Mode::kActivated, {{"Glow", 1.f}}, {{"Glow", 1.f}, {"Activated", 0.f}}) == "&s:Activated=0");
  // Tiny float noise is not a change; names compare without case.
  CHECK(Suffix(Mode::kAll, {{"Glow", 1000.f}}, {{"glow", 1000.001f}}).empty());
  CHECK(Suffix(Mode::kAll, {{"Glow", 1000.f}}, {{"glow", 1001.f}}) == "&s:glow=1001");
  CHECK(Suffix(Mode::kAll, {{"Glow", 0.f}}, {{"Glow", 0.5f}}) == "&s:Glow=0.5");

  // Hash: equal words equal, one differing word differs.
  uint64_t a = 1469598103934665603ull, b = a, c = a;
  for (uint32_t w : {1u, 0x3F800000u, 7u}) a = Mix(a, w);
  for (uint32_t w : {1u, 0x3F800000u, 7u}) b = Mix(b, w);
  for (uint32_t w : {1u, 0x3F800001u, 7u}) c = Mix(c, w);
  CHECK(a == b && a != c);

  // Churn guard: a window of changes passes up to the cap, the rest are refused, then the window turns over.
  ChurnGuard g;
  uint32_t allowed = 0;
  for (uint64_t f = 1; f <= 100; ++f) allowed += g.Allow(f);
  CHECK(allowed == ChurnGuard::kMaxChanges);
  CHECK(!g.Allow(599));
  CHECK(g.Allow(1 + ChurnGuard::kWindow));
  ChurnGuard h;
  allowed = 0;
  for (uint64_t f = 0; f < 20; ++f) allowed += h.Allow(f * 100);  // a change every 100 frames never trips it
  CHECK(allowed == 20);

  if (failures) return 1;
  std::printf("live_mic_test: ok\n");
  return 0;
}
