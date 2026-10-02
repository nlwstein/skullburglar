// BOTS_D3D_CENSUS=1: count calls into the game's statically linked Xbox D3D runtime (native renderer, P0).
//
// Every runtime function that code outside the runtime calls (src/gfx/d3d_entries.inc, from
// tools/d3d_scan.py) is overridden here: count the call and its return address, then run the original.
// Every 5 s the log gets the calls per second of each entry point and its busiest call sites, which is how
// the API surface gets named and ranked. Off by default; the cost when off is one predictable branch.
#include <cstdint>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace {

bool CensusOn() {
  static const bool on = [] {
    const char* v = std::getenv("BOTS_D3D_CENSUS");
    return v && *v == '1';
  }();
  return on;
}

struct Entry;
std::vector<Entry*>& Registry() {
  static std::vector<Entry*> entries;
  return entries;
}

void StartReporter();

struct Entry {
  explicit Entry(uint32_t a) : address(a) { Registry().push_back(this); }

  void Hit(uint32_t return_address) {
    if (calls.fetch_add(1, std::memory_order_relaxed) == 0) StartReporter();
    std::lock_guard lock(mutex);
    ++sites[return_address];
  }

  const uint32_t address;
  std::atomic<uint64_t> calls{0};
  uint64_t reported = 0;
  std::mutex mutex;
  std::unordered_map<uint32_t, uint64_t> sites;
};

void Report(double seconds) {
  struct Row {
    Entry* entry;
    uint64_t delta;
  };
  std::vector<Row> rows;
  for (Entry* e : Registry()) {
    const uint64_t now = e->calls.load(std::memory_order_relaxed);
    if (now != e->reported) rows.push_back({e, now - e->reported});
    e->reported = now;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.delta > b.delta; });
  REXLOG_INFO("d3d census: {} entry points active in the last {:.1f} s", rows.size(), seconds);
  for (const Row& r : rows) {
    std::vector<std::pair<uint32_t, uint64_t>> sites;
    {
      std::lock_guard lock(r.entry->mutex);
      sites.assign(r.entry->sites.begin(), r.entry->sites.end());
      r.entry->sites.clear();
    }
    std::sort(sites.begin(), sites.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string top;
    for (size_t i = 0; i < sites.size() && i < 4; ++i) {
      top += fmt::format(" {:08X}x{}", sites[i].first, sites[i].second);
    }
    REXLOG_INFO("d3d census: {:08X} {:9.1f}/s  sites {}:{}", r.entry->address, r.delta / seconds, sites.size(), top);
  }
}

void StartReporter() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      auto last = std::chrono::steady_clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const auto now = std::chrono::steady_clock::now();
        Report(std::chrono::duration<double>(now - last).count());
        last = now;
      }
    }).detach();
  });
}

}  // namespace

#define D3D_ENTRY(a)                                                                \
  extern "C" void __imp__sub_##a(PPCContext& __restrict ctx, uint8_t* base);        \
  static Entry census_##a(0x##a);                                                   \
  extern "C" void sub_##a(PPCContext& __restrict ctx, uint8_t* base) {              \
    if (CensusOn()) census_##a.Hit(uint32_t(ctx.lr));                               \
    __imp__sub_##a(ctx, base);                                                      \
  }
#include "d3d_entries.inc"
#undef D3D_ENTRY
