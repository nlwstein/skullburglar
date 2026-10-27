// UE3 garbage collection: timing, counts and cache warming (B-241).
//
// UObject::CollectGarbage (sub_825EA258) runs about every 30 s in play (UWorld::Tick, when
// TimeSinceLastPendingKillPurge passes GEngine->TimeBetweenPurgingPendingKillObjects) and on map changes. It:
//   1. finishes any pending incremental purge (IncrementalPurgeGarbage, sub_825E7EB0, no time limit);
//   2. PerformReachabilityAnalysis (sub_825E99F0): one pass over every object in GObjObjects from GObjFirstGCIndex
//      (count it, read its flags, call a virtual, mark it unreachable unless it is kept), then the reference-token walk
//      from the root set, which clears the mark on everything reachable;
//   3. one more pass over every object: ConditionalBeginDestroy (sub_8247D8B8) on each that is still unreachable;
// and the next frames' IncrementalPurgeGarbage calls (time-limited) finish destroying and free them.
// The passes over the object table are cache-miss bound on a laptop: each object is its own heap block, so every
// header read is a miss. The Xbox 360 code prefetches 10 objects ahead (dcbt), which the recompiler drops.
//
// Prefetch (default on, BOTS_GC_PREFETCH=0 turns it off): before the reachability analysis and before the
// BeginDestroy pass, the host prefetches the header line of every object in the table (a tight loop that issues many
// misses at once instead of one after another), so the game's own passes find them in the cache. It only reads
// guest memory and issues prefetch hints: what GC marks, keeps and frees is unchanged.
//   BOTS_GC_PREFETCH=0   off (the game's code alone)
//   BOTS_GC_PREFETCH=1   prefetch passes (default)
//
// Log (BOTS_GC_LOG, default 1; 0 silences it): one line per CollectGarbage ("gc: pass ..."; times, objects in the
// table, objects found unreachable), one when the purge that follows completes ("gc: purged ..."; live objects
// before and after), and one per game frame longer than BOTS_GC_SLOW_MS (default 50) with the GC time inside that
// frame ("gc: slow frame ..."), so long frames can be told apart into GC and not GC.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/context.h>

extern "C" void __imp__sub_825EA258(PPCContext& __restrict ctx, uint8_t* base);  // UObject::CollectGarbage
extern "C" void __imp__sub_825E99F0(PPCContext& __restrict ctx, uint8_t* base);  // PerformReachabilityAnalysis
extern "C" void __imp__sub_825E7EB0(PPCContext& __restrict ctx, uint8_t* base);  // IncrementalPurgeGarbage
extern "C" void __imp__sub_8247D8B8(PPCContext& __restrict ctx, uint8_t* base);  // UObject::ConditionalBeginDestroy

namespace bots::gc {
namespace {

constexpr uint32_t kObjects = 0x8380B15C;          // GObjObjects (TArray<UObject*>: data, num, max)
constexpr uint32_t kFirstGCIndex = 0x8380B130;     // GObjFirstGCIndex
constexpr uint32_t kPurgeRequired = 0x8380B128;    // GObjPurgeIsRequired
constexpr uint32_t kPurgeInProgress = 0x8380B120;  // GObjIncrementalPurgeIsInProgress
constexpr uint32_t kMarkCount = 0x8381CACC;        // GObjectCountDuringLastMarkPhase
constexpr uint32_t kGEngine = 0x8380D380;          // GEngine (UEngine*)
constexpr uint32_t kTimeBetweenPurging = 720;      // UEngine::TimeBetweenPurgingPendingKillObjects (float)

using clk = std::chrono::steady_clock;
const clk::time_point g_t0 = clk::now();
double NowMs() { return std::chrono::duration<double, std::milli>(clk::now() - g_t0).count(); }

uint8_t* Raw(uint8_t* base, uint32_t a) { return base + (uint64_t(a) + (a >= 0xE0000000u ? 0x1000u : 0u)); }
uint32_t U32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return __builtin_bswap32(v);
}
float F32(uint8_t* base, uint32_t a) {
  const uint32_t v = U32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

int EnvInt(const char* name, int fallback) {
  const char* e = std::getenv(name);
  return e && *e ? std::atoi(e) : fallback;
}
int PrefetchMode() {
  static const int v = EnvInt("BOTS_GC_PREFETCH", 1);
  return v;
}
bool LogOn() {
  static const bool v = EnvInt("BOTS_GC_LOG", 1) != 0;
  return v;
}
double SlowMs() {
  static const double v = EnvInt("BOTS_GC_SLOW_MS", 50);
  return v;
}

// Game thread only (CollectGarbage, the purge and the frame tick all run on it).
struct Pass {
  int n = 0;                  // passes so far
  bool in_gc = false;         // inside CollectGarbage
  double start = 0;           // CollectGarbage entry (ms since start)
  double mark_ms = 0, prepurge_ms = 0, prefetch_ms = 0;
  uint32_t unreachable = 0;   // ConditionalBeginDestroy calls inside CollectGarbage
  bool purge_pending = false;  // a purge started by this pass has not finished yet
  double purge_ms = 0;        // time-limited purge calls since the pass
  int purge_calls = 0;
  uint32_t live_after_gc = 0;
} g;
double g_frame_start = -1;
double g_gc_in_frame = 0;     // CollectGarbage + purge time inside the current game frame
int g_slow_frames = 0, g_slow_gc_frames = 0;

uint32_t LiveObjects(uint8_t* base) {
  const uint32_t data = U32(base, kObjects), num = U32(base, kObjects + 4);
  uint32_t live = 0;
  for (uint32_t i = 0; data && i < num; ++i) live += U32(base, data + 4 * i) != 0;
  return live;
}

// Prefetch the first line of every object's header (vtable +0, flags +8; Class +52 is on the next line for some),
// GObjFirstGCIndex onwards. Many prefetches are in flight at once, so this costs about one miss per ten objects.
void PrefetchObjects(uint8_t* base) {
  const double t = NowMs();
  const uint32_t data = U32(base, kObjects), num = U32(base, kObjects + 4);
  const uint32_t first = U32(base, kFirstGCIndex);
  if (!data || first >= num) return;
  const uint32_t* slots = reinterpret_cast<const uint32_t*>(Raw(base, data));
  for (uint32_t i = first; i < num; ++i) {
    const uint32_t o = __builtin_bswap32(slots[i]);
    if (!o) continue;
    const uint8_t* p = Raw(base, o);
    __builtin_prefetch(p, 0, 3);
    __builtin_prefetch(p + 52, 0, 3);
  }
  g.prefetch_ms += NowMs() - t;
}

}  // namespace

// Game thread, once per frame (the viewport input poll; game_hooks.cpp).
void Tick() {
  const double now = NowMs();
  if (g_frame_start >= 0) {
    const double ms = now - g_frame_start;
    if (ms > SlowMs()) {
      ++g_slow_frames;
      if (g_gc_in_frame > 0) ++g_slow_gc_frames;
      if (LogOn())
        REXLOG_INFO("gc: slow frame at {:.3f} s: {:.1f} ms, GC {:.1f} ms in it (slow frames {}, with GC {})",
                    now / 1000.0, ms, g_gc_in_frame, g_slow_frames, g_slow_gc_frames);
    }
  }
  g_frame_start = now;
  g_gc_in_frame = 0;
}

}  // namespace bots::gc

using namespace bots::gc;

// UObject::CollectGarbage(QWORD KeepFlags, UBOOL bPerformFullPurge)
extern "C" void sub_825EA258(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t caller = uint32_t(ctx.lr);
  const uint64_t keep = ctx.r3.u64;
  const bool full = ctx.r4.u32 != 0;
  g.in_gc = true;
  g.start = NowMs();
  g.mark_ms = g.prepurge_ms = g.prefetch_ms = 0;
  g.unreachable = 0;
  const bool was_pending = g.purge_pending;
  __imp__sub_825EA258(ctx, base);
  const double end = NowMs(), ms = end - g.start;
  g.in_gc = false;
  g_gc_in_frame += ms;
  ++g.n;
  g.purge_pending = U32(base, kPurgeRequired) != 0 || U32(base, kPurgeInProgress) != 0;
  g.purge_ms = 0;
  g.purge_calls = 0;
  if (!LogOn()) return;
  g.live_after_gc = LiveObjects(base);
  const uint32_t engine = U32(base, kGEngine);
  REXLOG_INFO("gc: pass {} at {:.3f} s: {:.1f} ms (finish previous purge {:.1f}, reachability {:.1f}, begin-destroy "
              "{:.1f}; prefetch {:.2f}), {} objects marked, {} unreachable, {} in table, keep {:016X}, full purge {}, "
              "caller {:08X}, interval {:.1f} s{}",
              g.n, g.start / 1000.0, ms, g.prepurge_ms, g.mark_ms, ms - g.mark_ms - g.prepurge_ms, g.prefetch_ms,
              U32(base, kMarkCount), g.unreachable, g.live_after_gc, keep, full ? 1 : 0, caller,
              engine ? F32(base, engine + kTimeBetweenPurging) : 0.0f,
              was_pending ? " (previous purge was still pending)" : "");
}

// PerformReachabilityAnalysis(TArray<UObject*>& ObjectsToSerialize, QWORD KeepFlags)
extern "C" void sub_825E99F0(PPCContext& __restrict ctx, uint8_t* base) {
  const double t = NowMs();
  if (PrefetchMode() == 1) PrefetchObjects(base);
  __imp__sub_825E99F0(ctx, base);
  g.mark_ms += NowMs() - t;
  // The BeginDestroy pass that follows walks the whole table again.
  if (PrefetchMode() == 1) PrefetchObjects(base);
}

// IncrementalPurgeGarbage(UBOOL bUseTimeLimit, FLOAT TimeLimit)
extern "C" void sub_825E7EB0(PPCContext& __restrict ctx, uint8_t* base) {
  const double t = NowMs();
  __imp__sub_825E7EB0(ctx, base);
  const double ms = NowMs() - t;
  if (g.in_gc) {
    g.prepurge_ms += ms;
    return;
  }
  g_gc_in_frame += ms;
  if (!g.purge_pending) return;
  g.purge_ms += ms;
  ++g.purge_calls;
  if (U32(base, kPurgeRequired) || U32(base, kPurgeInProgress)) return;
  g.purge_pending = false;
  if (LogOn())
    REXLOG_INFO("gc: purged after pass {} at {:.3f} s: {:.1f} ms over {} calls, live objects {} -> {}", g.n,
                NowMs() / 1000.0, g.purge_ms, g.purge_calls, g.live_after_gc, LiveObjects(base));
}

// UObject::ConditionalBeginDestroy()
extern "C" void sub_8247D8B8(PPCContext& __restrict ctx, uint8_t* base) {
  if (g.in_gc) ++g.unreachable;
  __imp__sub_8247D8B8(ctx, base);
}
