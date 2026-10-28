// UE3 garbage collection: timing and counts, plus an optional host reachability analysis (B-241).
//
// UObject::CollectGarbage (sub_825EA258) runs about every 30 s in play (UWorld::Tick, when
// TimeSinceLastPendingKillPurge passes GEngine->TimeBetweenPurgingPendingKillObjects) and on map changes. It:
//   1. finishes any pending incremental purge (IncrementalPurgeGarbage, sub_825E7EB0, no time limit);
//   2. PerformReachabilityAnalysis (sub_825E99F0): one pass over every object in GObjObjects from GObjFirstGCIndex
//      (count it, read its flags, call a virtual, mark it unreachable unless it is kept), then the reference-token walk
//      from the root set, which clears the mark on everything reachable;
//   3. one more pass over every object: ConditionalBeginDestroy (sub_8247D8B8) on each that is still unreachable;
// and the next frames' IncrementalPurgeGarbage calls (time-limited) finish destroying and free them.
// Almost all of a pass is the reference-token walk: for each reachable object (~41k in a level), a virtual call and
// a read of every reference field (~14 tokens, ~6 references per object) and of the referenced objects' flags.
//
// Host analysis (on by default; BOTS_GC_HOST=0 runs the game's own): sub_825E99F0 replaced by a host
// implementation of the same algorithm (see HostReachability). It gives the same results (BOTS_GC_VERIFY). On a
// laptop it cut the periodic pass from about 28 to 22 ms in whole-run A/B tests; software prefetch and a helper
// thread warming the cache did not help further and were dropped. The walk itself runs on several threads (B-252,
// ParallelWalk below): the pass stays stop-the-world but takes about 14 ms instead of 23 ms on the same laptop.
//   BOTS_GC_HOST=0     the game's own analysis
//   BOTS_GC_HOST=1     the host analysis (default)
//   BOTS_GC_HOST=2     (measuring) alternate the host analysis and the game's on successive passes
//   BOTS_GC_PAR=0      (with the host analysis) walk on the game thread only; the default splits the walk across threads
//   BOTS_GC_THREADS=n  threads for the parallel walk, the game thread included (default: up to 4 of the logical CPUs)
//   BOTS_GC_VERIFY=1   (with BOTS_GC_HOST=1) run both on every pass and log whether they agree ("gc: verify")
//   BOTS_GC_PROF=1     (with the host analysis) also time the per-object virtual calls ("gc: walk")
//
// Log (BOTS_GC_LOG, default 1; 0 silences it): one line per CollectGarbage ("gc: pass ..."; times, objects in the
// table, objects found unreachable), one when the purge that follows completes ("gc: purged ..."; live objects
// before and after), and one per game frame longer than BOTS_GC_SLOW_MS (default 50) with the GC time inside that
// frame ("gc: slow frame ..."), so long frames can be told apart into GC and not GC.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

extern "C" void __imp__sub_825EA258(PPCContext& __restrict ctx, uint8_t* base);  // UObject::CollectGarbage
extern "C" void __imp__sub_825E99F0(PPCContext& __restrict ctx, uint8_t* base);  // PerformReachabilityAnalysis
extern "C" void __imp__sub_825E7EB0(PPCContext& __restrict ctx, uint8_t* base);  // IncrementalPurgeGarbage
extern "C" void __imp__sub_8247D8B8(PPCContext& __restrict ctx, uint8_t* base);  // UObject::ConditionalBeginDestroy
// Guest helpers the reachability analysis calls.
extern "C" void sub_82481488(PPCContext& __restrict ctx, uint8_t* base);  // TArray<UObject*>::Empty(slack)
extern "C" void sub_82481508(PPCContext& __restrict ctx, uint8_t* base);  // TArray<UObject*>::AddItem(const T&)
extern "C" void sub_82548A78(PPCContext& __restrict ctx, uint8_t* base);  // FArray::CalculateSlack(elem size)
extern "C" void sub_8234D8D8(PPCContext& __restrict ctx, uint8_t* base);  // FArray::Realloc(elem size, align)
extern "C" void sub_82350DD8(PPCContext& __restrict ctx, uint8_t* base);  // GMalloc first-use init
extern "C" void sub_824B7E88(PPCContext& __restrict ctx, uint8_t* base);  // UClass::StaticClass (lazy)
extern "C" void sub_824B7F60(PPCContext& __restrict ctx, uint8_t* base);  // its registration
extern "C" void sub_825E9990(PPCContext& __restrict ctx, uint8_t* base);  // UClass::AssembleReferenceTokenStream

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
int HostMode() {
  static const int v = EnvInt("BOTS_GC_HOST", 1);
  return v;
}
bool HostOn() { return HostMode() != 0; }
// Parallel walk (B-252): the reference walk split across threads. BOTS_GC_PAR=0 uses the single-threaded host walk,
// BOTS_GC_THREADS=n picks the thread count (default: up to 4, the logical CPUs permitting; 1 runs the same code on
// the game thread alone).
bool ParOn() {
  static const bool v = EnvInt("BOTS_GC_PAR", 1) != 0;
  return v;
}
bool VerifyOn() {
  static const bool v = EnvInt("BOTS_GC_VERIFY", 0) != 0;
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
  double mark_ms = 0, prepurge_ms = 0;
  double table_ms = 0;         // host analysis: the pass over the object table (the rest of mark_ms is the walk)
  bool host = false;          // this pass used the host analysis
  uint64_t walk_objs = 0, walk_tokens = 0, walk_refs = 0;  // host walk counts
  double virt_ms = 0;         // BOTS_GC_PROF=1: time in the per-object virtual (vtable +244)
  uint32_t unreachable = 0;   // ConditionalBeginDestroy calls inside CollectGarbage
  bool purge_pending = false;  // a purge started by this pass has not finished yet
  double purge_ms = 0;        // time-limited purge calls since the pass
  int purge_calls = 0;
  uint32_t live_after_gc = 0;
  uint32_t par_levels = 0, par_parallel_levels = 0, par_deferred = 0;  // parallel walk
  int par_threads = 0;
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

// ---- Host PerformReachabilityAnalysis --------------------------------------------------------------------------
// The same algorithm as sub_825E99F0: the same loads and stores of guest memory in the same order, the same guest calls
// (virtuals, TArray growth through the game's allocator, AddItem) with the same arguments and return addresses. Only
// the token interpreter is native. Like the Xbox 360 code, the object-table pass prefetches 10 objects ahead.

constexpr uint32_t kPoolStart = 0x8380B138;  // GPermanentObjectPool (objects in it are never collected)
constexpr uint32_t kPoolEnd = 0x8380B13C;
constexpr uint32_t kUClass = 0x8380D280;     // UClass::StaticClass() cache
constexpr uint32_t kGMalloc = 0x837D4F88;    // GMalloc (FMalloc*)
constexpr uint64_t kPendingKill = 1ull << 61;
constexpr uint64_t kUnreachable = 1ull << 33;

void S32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, a), &v, 4);
}
uint64_t U64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, Raw(base, a), 8);
  return __builtin_bswap64(v);
}
void S64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(Raw(base, a), &v, 8);
}
uint8_t U8(uint8_t* base, uint32_t a) { return *Raw(base, a); }
void Prefetch(uint8_t* base, uint32_t a) { __builtin_prefetch(Raw(base, a), 0, 3); }

// Guest function pointers by address: a small direct-mapped cache in front of the dispatcher.
PPCFunc* Resolve(uint32_t target) {
  struct Slot {
    uint32_t addr;
    PPCFunc* fn;
  };
  static Slot cache[512];
  Slot& s = cache[(target >> 2) & 511];
  if (s.addr != target || !s.fn) s = {target, rex::runtime::ResolveIndirectFunction(target)};
  return s.fn;
}
void CallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target, uint32_t lr) {
  ctx.lr = lr;
  Resolve(target)(ctx, base);
}

// For BOTS_GC_VERIFY: references the host pass nulled (address, old value), so a verifying pass can undo them.
struct Undo {
  bool on = false;
  std::vector<std::pair<uint32_t, uint32_t>> words;
  std::vector<std::pair<uint32_t, uint64_t>> dwords;
} g_undo;
void NullRef(uint8_t* base, uint32_t at) {
  if (g_undo.on) g_undo.words.push_back({at, U32(base, at)});
  S32(base, at, 0);
}

// ObjectsToSerialize.AddItem(obj), as the token loop inlines it: grow through CalculateSlack + GMalloc->Realloc.
void AddInline(PPCContext& ctx, uint8_t* base, uint32_t arr, uint32_t field, uint32_t lr_slack, uint32_t lr_realloc) {
  const uint32_t max = U32(base, arr + 8), n = U32(base, arr + 4);
  S32(base, arr + 4, n + 1);
  if (int32_t(n + 1) > int32_t(max)) {
    ctx.r3.u64 = arr;
    ctx.r4.u64 = 4;
    ctx.lr = lr_slack;
    sub_82548A78(ctx, base);
    const uint32_t newmax = ctx.r3.u32, data = U32(base, arr);
    S32(base, arr + 8, newmax);
    if (data || newmax) {
      uint32_t m = U32(base, kGMalloc);
      if (!m) {
        ctx.lr = lr_realloc;
        sub_82350DD8(ctx, base);
        m = U32(base, kGMalloc);
      }
      ctx.r3.u64 = m;
      ctx.r4.u64 = data;
      ctx.r5.u64 = uint64_t(newmax) << 2 & 0xFFFFFFFCull;
      ctx.r6.u64 = 8;
      CallIndirect(ctx, base, U32(base, U32(base, m) + 8), lr_realloc);
      S32(base, arr, ctx.r3.u32);
    }
  }
  const uint32_t slot = U32(base, arr) + n * 4;
  if (slot) S32(base, slot, U32(base, field));
}


// ---- Parallel reference walk (B-252) ---------------------------------------------------------------------------------
// The walk is memory-bound pointer chasing over ~41k objects (22-30 ms on a laptop CPU), so several threads, each with
// its own cache misses in flight, finish it sooner. The game thread waits for them: this is still a stop-the-world
// pass, just a shorter one. Level by level (breadth first), so nothing guest-side runs during a parallel phase:
//   phase V  workers find the objects in the level whose per-object virtual (vtable+244) does something. A virtual
//            whose first instruction is `blr` does nothing and is skipped; the others run on the game thread, in
//            the game's own way, between the phases (they may append to the list through guest code).
//   phase W  workers walk each object's reference tokens (the same interpreter as HostReachability). The
//            unreachable bit is cleared with an atomic AND, so exactly one thread claims each object; claimed objects go
//            to a per-thread list. References to objects with RF_PendingKill are nulled by the thread that owns the
//            referring object (each object is walked once, so no two threads write the same field).
//   merge    the game thread appends every claimed object to ObjectsToSerialize (the game's allocator), and that is the
//            next level.
// Same set of marks as the game's walk (checked by BOTS_GC_VERIFY); the order of the list differs, and nothing reads
// the order afterwards.

struct Frame {
  uint32_t cur, stride, count, loop;
};
struct Wk {
  std::vector<uint32_t> found, deferred;
  std::vector<std::pair<uint32_t, uint32_t>> nulled;  // BOTS_GC_VERIFY undo: (address, old word)
  std::vector<std::pair<uint32_t, uint64_t>> zeroed;  // (address, old dword)
  std::vector<Frame> frames;
  std::unordered_map<uint32_t, uint32_t> hist;  // BOTS_GC_PROF: virtual target -> objects
  uint64_t objs = 0, toks = 0, refs = 0;
  void Reset() {
    found.clear();
    deferred.clear();
    nulled.clear();
    zeroed.clear();
  }
};

void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ volatile("yield");
#endif
}

// Clears RF_Unreachable (bit 33: byte 3 of the big-endian flags, mask 2) and says whether this call did it.
bool Claim(uint8_t* base, uint32_t r) {
  return (__atomic_fetch_and(Raw(base, r + 8 + 3), uint8_t(~0x02), __ATOMIC_ACQ_REL) & 0x02) != 0;
}
bool PendingKill(uint8_t* base, uint32_t r) { return (U8(base, r + 8) & 0x20) != 0; }  // bit 61
bool Unreachable(uint8_t* base, uint32_t r) { return (U8(base, r + 8 + 3) & 0x02) != 0; }

struct Pool {
  struct Job {
    int phase = 0;  // 0: find virtuals, 1: walk
    uint32_t data = 0, end = 0;
    bool undo = false, prof = false;
    uint32_t pool0 = 0, pool1 = 0;
  } job;
  std::atomic<uint32_t> cursor{0};
  std::atomic<uint32_t> epoch{0};
  std::atomic<int> finished{0};
  std::atomic<bool> active{false};
  std::mutex m;
  std::condition_variable cv;
  bool quit = false;
  uint8_t* base = nullptr;
  std::vector<std::thread> threads;
  std::vector<std::unique_ptr<Wk>> wk;  // [0] is the game thread's
  int total = 1;
};
Pool* g_pool = nullptr;

constexpr uint32_t kChunk = 64;

void WalkObject(Pool& P, Wk& w, uint32_t obj) {
  uint8_t* base = P.base;
  const bool undo = P.job.undo;
  auto in_pool = [&](uint32_t r) { return !(r < P.job.pool0) && r <= P.job.pool1; };
  auto null_ref = [&](uint32_t at) {
    if (undo) w.nulled.push_back({at, U32(base, at)});
    S32(base, at, 0);
  };
  ++w.objs;
  std::vector<Frame>& fr = w.frames;
  fr.clear();
  fr.push_back({obj, 0, ~0u, ~0u});
  uint32_t cur = obj, tok = 0, returns = 0;
  const uint32_t tokens = U32(base, U32(base, obj + 52) + 480);
  for (;;) {
    for (uint32_t r = 0; int32_t(r) < int32_t(returns);) {
      Frame& f = fr.back();
      const uint32_t c = f.count - 1;
      f.count = c;
      if (int32_t(c) > 0) {
        tok = f.loop;
        cur = f.stride + f.cur;
        f.cur = cur;
        break;
      }
      if (fr.size() > 1) fr.pop_back();
      ++r;
      cur = fr.back().cur;
    }
    const uint32_t t = U32(base, tokens + 4 * tok);
    ++w.toks;
    ++tok;
    const uint32_t type = t >> 16 & 0xFF, off = t & 0xFFFF;
    if (type == 1 || type == 2) {
      returns = t >> 24;
      const uint32_t at = cur + off, r = U32(base, at);
      if (!r || in_pool(r)) continue;
      ++w.refs;
      if (type == 1 && PendingKill(base, r)) {
        null_ref(at);
        continue;
      }
      if (!Unreachable(base, r) || !Claim(base, r)) continue;
      w.found.push_back(r);
    } else if (type == 3) {
      returns = t >> 24;
      const uint32_t a = cur + off;
      for (uint32_t j = 0; int32_t(j) < int32_t(U32(base, a + 4)); ++j) {
        const uint32_t at = U32(base, a) + 4 * j, r = U32(base, at);
        if (!r || in_pool(r)) continue;
        ++w.refs;
        if (PendingKill(base, r)) {
          null_ref(at);
          continue;
        }
        if (!Unreachable(base, r) || !Claim(base, r)) continue;
        w.found.push_back(r);
      }
    } else if (type == 4) {
      const uint32_t a = cur + off;
      const uint32_t stride = U32(base, tokens + 4 * tok), skip = U32(base, tokens + 4 * (tok + 1));
      const uint32_t after = tok + 2;
      const uint32_t n = U32(base, a + 4);
      cur = U32(base, a);
      fr.push_back({cur, stride, n, after});
      if (n != 0) {
        tok = after;
        returns = 0;
      } else {
        const uint32_t sk = ((skip + tok + 1) & 0x00FFFFFFu) | (skip & 0xFF000000u);
        tok = sk & 0xFFFFFF;
        returns = uint32_t(U8(base, tokens + 4 * tok - 4)) - (sk >> 24);
      }
    } else if (type == 5) {
      const uint32_t stride = U32(base, tokens + 4 * tok), n = U32(base, tokens + 4 * (tok + 1));
      tok += 2;
      fr.push_back({cur, stride, n, tok});
      returns = 0;
    } else if (type == 6) {
      break;
    } else if (type == 7) {
      returns = t >> 24;
      const uint32_t at = cur + off, r = U32(base, at);
      const bool was_null = r == 0;
      if (r && !in_pool(r)) {
        if (PendingKill(base, r)) {
          null_ref(at);
        } else if (Unreachable(base, r) && Claim(base, r)) {
          w.found.push_back(r);
        }
      }
      if (!was_null && !U32(base, at)) {
        if (undo) w.zeroed.push_back({at + 4, U64(base, at + 4)});
        S64(base, at + 4, 0);
      }
    }
  }
}

void RunJob(Pool& P, Wk& w) {
  uint8_t* base = P.base;
  for (;;) {
    const uint32_t i = P.cursor.fetch_add(kChunk, std::memory_order_relaxed);
    if (i >= P.job.end) break;
    const uint32_t e = std::min(i + kChunk, P.job.end);
    for (uint32_t k = i; k < e; ++k) {
      const uint32_t obj = U32(base, P.job.data + 4 * k);
      if (P.job.phase == 0) {
        const uint32_t target = U32(base, U32(base, obj) + 244);
        if (P.job.prof) ++w.hist[target];
        if (U32(base, target) != 0x4E800020u) w.deferred.push_back(obj);  // not a bare `blr`
      } else {
        WalkObject(P, w, obj);
      }
    }
  }
}

void WorkerMain(Pool* P, int id, uint32_t seen) {
  Wk& w = *P->wk[id];
  for (;;) {
    {
      std::unique_lock<std::mutex> l(P->m);
      P->cv.wait(l, [&] { return P->active.load() || P->quit; });
      if (P->quit) return;
    }
    int spins = 0;
    while (P->active.load(std::memory_order_acquire)) {
      const uint32_t e = P->epoch.load(std::memory_order_acquire);
      if (e == seen) {
        if (++spins < 20000) CpuRelax(); else std::this_thread::yield();
        continue;
      }
      seen = e;
      spins = 0;
      RunJob(*P, w);
      P->finished.fetch_add(1, std::memory_order_release);
    }
  }
}

int ThreadCount() {
  static const int v = [] {
    int n = EnvInt("BOTS_GC_THREADS", 0);
    if (n <= 0) n = int(std::min(4u, std::max(1u, std::thread::hardware_concurrency())));
    return std::min(n, 16);
  }();
  return v;
}

Pool& GetPool(uint8_t* base) {
  if (!g_pool) {
    g_pool = new Pool();  // lives for the process; the workers are detached at exit
    Pool& P = *g_pool;
    P.base = base;
    P.total = ThreadCount();
    for (int i = 0; i < P.total; ++i) P.wk.push_back(std::make_unique<Wk>());
    for (int i = 1; i < P.total; ++i) {
      std::thread t(WorkerMain, &P, i, 0u);
      t.detach();
    }
  }
  return *g_pool;
}

// Runs one phase over arr[begin, end) on all threads (or on the game thread alone when it is small).
void RunPhase(Pool& P, int phase, uint32_t begin, uint32_t end, bool parallel) {
  P.job.phase = phase;
  P.job.end = end;
  P.cursor.store(begin, std::memory_order_relaxed);
  if (!parallel || P.total == 1) {
    RunJob(P, *P.wk[0]);
    return;
  }
  P.finished.store(0, std::memory_order_relaxed);
  P.epoch.fetch_add(1, std::memory_order_release);
  RunJob(P, *P.wk[0]);
  while (P.finished.load(std::memory_order_acquire) < P.total - 1) CpuRelax();
}

void ParallelWalk(PPCContext& ctx, uint8_t* base, uint32_t arr) {
  static const bool kProf = EnvInt("BOTS_GC_PROF", 0) != 0;
  Pool& P = GetPool(base);
  P.job.undo = g_undo.on;
  P.job.prof = kProf;
  P.job.pool0 = U32(base, kPoolStart);
  P.job.pool1 = U32(base, kPoolEnd);
  for (auto& w : P.wk) {
    w->Reset();
    w->objs = w->toks = w->refs = 0;
    w->hist.clear();
  }
  if (P.total > 1) {
    {
      std::lock_guard<std::mutex> l(P.m);
      P.active.store(true, std::memory_order_release);
    }
    P.cv.notify_all();
  }
  uint32_t begin = 0, levels = 0, deferred = 0, parallel_levels = 0;
  for (;;) {
    const uint32_t end = U32(base, arr + 4);
    if (int32_t(end) <= int32_t(begin)) break;
    ++levels;
    const bool par = end - begin >= 192;
    parallel_levels += par;
    P.job.data = U32(base, arr);
    RunPhase(P, 0, begin, end, par);
    for (auto& w : P.wk) {
      for (const uint32_t obj : w->deferred) {
        ctx.r3.u64 = obj;
        ctx.r4.u64 = arr;
        CallIndirect(ctx, base, U32(base, U32(base, obj) + 244), 0x825E9CAC);
        ++deferred;
      }
      w->deferred.clear();
    }
    P.job.data = U32(base, arr);  // a virtual may have grown the list
    RunPhase(P, 1, begin, end, par);
    for (auto& w : P.wk) {
      for (const uint32_t r : w->found) {
        // as the token loop appends: grow through CalculateSlack + GMalloc->Realloc, then store the slot
        const uint32_t n = U32(base, arr + 4), max = U32(base, arr + 8);
        S32(base, arr + 4, n + 1);
        if (int32_t(n + 1) > int32_t(max)) {
          ctx.r3.u64 = arr;
          ctx.r4.u64 = 4;
          ctx.lr = 0x825E9DE4;
          sub_82548A78(ctx, base);
          const uint32_t newmax = ctx.r3.u32, data = U32(base, arr);
          S32(base, arr + 8, newmax);
          if (data || newmax) {
            uint32_t m = U32(base, kGMalloc);
            if (!m) {
              ctx.lr = 0x825E9E2C;
              sub_82350DD8(ctx, base);
              m = U32(base, kGMalloc);
            }
            ctx.r3.u64 = m;
            ctx.r4.u64 = data;
            ctx.r5.u64 = uint64_t(newmax) << 2 & 0xFFFFFFFCull;
            ctx.r6.u64 = 8;
            CallIndirect(ctx, base, U32(base, U32(base, m) + 8), 0x825E9E2C);
            S32(base, arr, ctx.r3.u32);
          }
        }
        const uint32_t slot = U32(base, arr) + n * 4;
        if (slot) S32(base, slot, r);
      }
      w->found.clear();
    }
    begin = end;
  }
  if (P.total > 1) P.active.store(false, std::memory_order_release);
  uint64_t objs = 0, toks = 0, refs = 0;
  for (auto& w : P.wk) {
    objs += w->objs;
    toks += w->toks;
    refs += w->refs;
    if (g_undo.on) {
      g_undo.words.insert(g_undo.words.end(), w->nulled.begin(), w->nulled.end());
      g_undo.dwords.insert(g_undo.dwords.end(), w->zeroed.begin(), w->zeroed.end());
    }
  }
  g.walk_objs += objs;
  g.walk_tokens += toks;
  g.walk_refs += refs;
  g.par_levels = levels;
  g.par_parallel_levels = parallel_levels;
  g.par_deferred = deferred;
  g.par_threads = P.total;
  if (kProf && LogOn()) {
    std::unordered_map<uint32_t, uint32_t> all;
    for (auto& w : P.wk)
      for (auto& kv : w->hist) all[kv.first] += kv.second;
    std::vector<std::pair<uint32_t, uint32_t>> v(all.begin(), all.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string s;
    for (size_t i = 0; i < v.size() && i < 10; ++i)
      s += fmt::format(" {:08X}[first {:08X}]x{}", v[i].first, U32(base, v[i].first), v[i].second);
    REXLOG_INFO("gc: virtual targets (vtable+244):{}", s);
  }
}

void HostReachability(PPCContext& ctx, uint8_t* base) {
  const double t_table = NowMs();
  const uint32_t arr = ctx.r3.u32;  // TArray<UObject*>& ObjectsToSerialize
  const uint64_t keep = ctx.r4.u64;  // KeepFlags
  const uint32_t sp_in = ctx.r1.u32;
  const uint32_t sp = sp_in - 272;   // the original's frame: +80 scratch, +88 zero pair, +96 the token stack TArray
  S32(base, sp, sp_in);
  ctx.r1.u64 = sp;

  S32(base, arr + 12, 0);
  S32(base, kMarkCount, 0);
  ctx.r3.u64 = arr;
  ctx.r4.u64 = uint64_t(int64_t(int32_t(U32(base, kObjects + 4) - U32(base, kFirstGCIndex) + 2)));
  ctx.lr = 0x825E9A38;
  sub_82481488(ctx, base);
  uint32_t uclass = U32(base, kUClass);
  if (!uclass) {
    ctx.r3.u64 = 0x820010BC;
    ctx.lr = 0x825E9A54;
    sub_824B7E88(ctx, base);
    S32(base, kUClass, ctx.r3.u32);
    ctx.lr = 0x825E9A5C;
    sub_824B7F60(ctx, base);
    uclass = U32(base, kUClass);
  }

  // Pass 1: every object from GObjFirstGCIndex: count it; keep the root set and KeepFlags objects, mark the rest
  // unreachable; build the token streams of classes that have none yet.
  {
    int32_t i = int32_t(U32(base, kFirstGCIndex));
    uint32_t num = U32(base, kObjects + 4);
    while (i < int32_t(num)) {
      const uint32_t data = U32(base, kObjects);
      if (i + 10 < int32_t(num)) {
        const uint32_t ahead = U32(base, data + 4 * uint32_t(i) + 40);
        Prefetch(base, ahead);
        Prefetch(base, ahead + 52);
      }
      const uint32_t obj = U32(base, data + 4 * uint32_t(i));
      S32(base, sp + 80, obj);
      if (obj) {
        S32(base, kMarkCount, U32(base, kMarkCount) + 1);
        if (U64(base, obj + 8) & 0x4000) {  // RF_RootSet
          const uint32_t n = U32(base, arr + 4), max = U32(base, arr + 8);
          S32(base, arr + 4, n + 1);
          if (int32_t(n + 1) > int32_t(max)) {
            ctx.r3.u64 = arr;
            ctx.r4.u64 = 4;
            ctx.lr = 0x825E9AF8;
            sub_82548A78(ctx, base);
            S32(base, arr + 8, ctx.r3.u32);
            ctx.r3.u64 = arr;
            ctx.r4.u64 = 4;
            ctx.r5.u64 = 8;
            ctx.lr = 0x825E9B0C;
            sub_8234D8D8(ctx, base);
          }
          const uint32_t slot = U32(base, arr) + n * 4;
          if (slot) S32(base, slot, obj);
        } else {
          ctx.r3.u64 = obj;
          CallIndirect(ctx, base, U32(base, U32(base, obj) + 52), 0x825E9B38);
          if (ctx.r3.u32) S64(base, obj + 8, U64(base, obj + 8) | kPendingKill);
          const uint64_t f = U64(base, obj + 8);
          if (((f & keep) || keep == ~0ull) && !(f & kPendingKill)) {
            ctx.r3.u64 = arr;
            ctx.r4.u64 = sp + 80;
            ctx.lr = 0x825E9B84;
            sub_82481508(ctx, base);
          } else {
            S64(base, obj + 8, U64(base, obj + 8) | kUnreachable);
          }
        }
        if (U32(base, obj + 52) == uclass && !(U64(base, obj + 8) & 0x1000)) {
          ctx.r3.u64 = obj;
          ctx.lr = 0x825E9BB8;
          sub_825E9990(ctx, base);
        }
        num = U32(base, kObjects + 4);
      }
      ++i;
    }
  }

  g.table_ms += NowMs() - t_table;
  if (ParOn()) {
    ParallelWalk(ctx, base, arr);
    ctx.r1.u64 = sp_in;
    return;
  }
  // The token stack: a TArray of 16-byte frames {base, stride, count, loop token}, 128 of them up front.
  const uint32_t stack = sp + 96;
  S32(base, stack, 0);
  S32(base, stack + 8, 0);
  S32(base, stack + 4, 128);
  ctx.r3.u64 = stack;
  ctx.r4.u64 = 16;
  ctx.lr = 0x825E9BEC;
  sub_82548A78(ctx, base);
  {
    const uint32_t old = U32(base, stack), max = ctx.r3.u32;
    S32(base, stack + 8, max);
    if (old || max) {
      uint32_t m = U32(base, kGMalloc);
      if (!m) {
        ctx.lr = 0x825E9C1C;
        sub_82350DD8(ctx, base);
        m = U32(base, kGMalloc);
      }
      ctx.r3.u64 = m;
      ctx.r4.u64 = old;
      ctx.r5.u64 = uint64_t(max) << 4 & 0xFFFFFFF0ull;
      ctx.r6.u64 = 8;
      CallIndirect(ctx, base, U32(base, U32(base, m) + 8), 0x825E9C44);
      S32(base, stack, ctx.r3.u32);
    }
  }

  // Pass 2: the reference-token walk. Every object reached is cleared of RF_Unreachable and appended.
  const uint32_t pool0 = U32(base, kPoolStart), pool1 = U32(base, kPoolEnd);
  // GPermanentObjectPool bounds are re-read per reference by the original; nothing in the walk can change them.
  auto in_pool = [&](uint32_t r) { return !(r < pool0) && r <= pool1; };
  if (int32_t(U32(base, arr + 4)) > 0) {
    uint32_t next = 0;  // index of the next object to serialize
    for (;;) {
      const uint32_t data = U32(base, arr);
      const uint32_t obj = U32(base, data + 4 * next);
      ++next;
      static const bool kProf = EnvInt("BOTS_GC_PROF", 0) != 0;
      const double tv = kProf ? NowMs() : 0;
      ctx.r3.u64 = obj;
      ctx.r4.u64 = arr;
      CallIndirect(ctx, base, U32(base, U32(base, obj) + 244), 0x825E9CAC);
      if (kProf) g.virt_ms += NowMs() - tv;
      ++g.walk_objs;
      uint32_t frame = U32(base, stack);
      uint32_t cur = obj;  // r23: the struct the current tokens address
      uint32_t tok = 0;    // r22
      S32(base, frame, obj);
      S32(base, frame + 4, 0);
      S32(base, frame + 8, ~0u);
      S32(base, frame + 12, ~0u);
      const uint32_t tokens = U32(base, U32(base, obj + 52) + 480);
      uint32_t returns = 0;  // r21
      for (;;) {
        // Leave `returns` levels: each one either loops (next element of an array of structs) or pops its frame.
        for (uint32_t r = 0; int32_t(r) < int32_t(returns);) {
          const uint32_t c = U32(base, frame + 8) - 1;
          S32(base, frame + 8, c);
          if (int32_t(c) > 0) {
            tok = U32(base, frame + 12);
            cur = U32(base, frame + 4) + U32(base, frame);
            S32(base, frame, cur);
            break;
          }
          frame -= 16;
          ++r;
          cur = U32(base, frame);
        }
        const uint32_t t = U32(base, tokens + 4 * tok);
        ++g.walk_tokens;
        ++tok;
        S32(base, sp + 80, t);
        const uint32_t type = t >> 16 & 0xFF, off = t & 0xFFFF;
        if (type == 1 || type == 2) {  // an object reference (2: never nulled)
          returns = t >> 24;
          const uint32_t at = cur + off, r = U32(base, at);
          if (!r || in_pool(r)) continue;
          ++g.walk_refs;
          const uint64_t f = U64(base, r + 8);
          if (type == 1 && (f & kPendingKill)) {
            NullRef(base, at);
            continue;
          }
          if (!(f & kUnreachable)) continue;
          S64(base, r + 8, (type == 1 ? U64(base, r + 8) : f) & ~kUnreachable);
          if (type == 1)
            AddInline(ctx, base, arr, at, 0x825E9DE4, 0x825E9E2C);
          else
            AddInline(ctx, base, arr, at, 0x825EA098, 0x825EA0E0);
        } else if (type == 3) {  // TArray<UObject*>
          returns = t >> 24;
          const uint32_t a = cur + off;
          if (int32_t(U32(base, a + 4)) <= 0) continue;
          for (uint32_t j = 0; int32_t(j) < int32_t(U32(base, a + 4)); ++j) {
            const uint32_t at = U32(base, a) + 4 * j, r = U32(base, at);
            if (!r || in_pool(r)) continue;
            ++g.walk_refs;
            const uint64_t f = U64(base, r + 8);
            if (f & kPendingKill) {
              NullRef(base, at);
              continue;
            }
            if (!(f & kUnreachable)) continue;
            S64(base, r + 8, U64(base, r + 8) & ~kUnreachable);
            AddInline(ctx, base, arr, at, 0x825E9F18, 0x825E9F60);
          }
        } else if (type == 4) {  // TArray of structs: push a frame over its elements
          const uint32_t a = cur + off;
          const uint32_t stride = U32(base, tokens + 4 * tok), skip = U32(base, tokens + 4 * (tok + 1));
          const uint32_t after = tok + 2;
          const uint32_t n = U32(base, a + 4);
          cur = U32(base, a);
          frame += 16;
          S32(base, frame + 12, after);
          S32(base, frame + 4, stride);
          S32(base, frame + 8, n);
          S32(base, frame, cur);
          if (n != 0) {
            tok = after;
            returns = 0;
          } else {
            const uint32_t sk = ((skip + tok + 1) & 0x00FFFFFFu) | (skip & 0xFF000000u);
            tok = sk & 0xFFFFFF;
            returns = uint32_t(U8(base, tokens + 4 * tok - 4)) - (sk >> 24);
          }
        } else if (type == 5) {  // fixed array: push a frame
          const uint32_t stride = U32(base, tokens + 4 * tok), n = U32(base, tokens + 4 * (tok + 1));
          tok += 2;
          frame += 16;
          S32(base, frame, cur);
          S32(base, frame + 12, tok);
          S32(base, frame + 4, stride);
          S32(base, frame + 8, n);
          returns = 0;
        } else if (type == 6) {  // end of stream
          break;
        } else if (type == 7) {  // object + interface pointer
          returns = t >> 24;
          const uint32_t at = cur + off, r = U32(base, at);
          const bool was_null = r == 0;
          if (r && !in_pool(r)) {
            const uint64_t f = U64(base, r + 8);
            if (f & kPendingKill) {
              NullRef(base, at);
            } else if (f & kUnreachable) {
              S64(base, r + 8, U64(base, r + 8) & ~kUnreachable);
              ctx.r3.u64 = arr;
              ctx.r4.u64 = at;
              ctx.lr = 0x825EA1E0;
              sub_82481508(ctx, base);
            }
          }
          if (!was_null && !U32(base, at)) {
            S32(base, sp + 88, 0);
            S32(base, sp + 92, 0);
            if (g_undo.on) g_undo.dwords.push_back({at + 4, U64(base, at + 4)});
            S64(base, at + 4, U64(base, sp + 88));
          }
        }
        // Any other type: nothing, and `returns` keeps its value (as in the original).
      }
      if (int32_t(next) >= int32_t(U32(base, arr + 4))) break;
    }
  }
  if (const uint32_t data = U32(base, stack)) {
    uint32_t m = U32(base, kGMalloc);
    if (!m) {
      ctx.lr = 0x825EA234;
      sub_82350DD8(ctx, base);
      m = U32(base, kGMalloc);
    }
    ctx.r3.u64 = m;
    ctx.r4.u64 = data;
    CallIndirect(ctx, base, U32(base, U32(base, m) + 12), 0x825EA24C);
  }
  ctx.r1.u64 = sp_in;
}

// BOTS_GC_VERIFY=1: run the host analysis, record what it did, undo it, run the game's own code from the same state and
// compare: the ObjectsToSerialize list (same objects in the same order), every object's RF_Unreachable and
// RF_PendingKill bits, and the references the host nulled. The game's result is the one that stays.
void VerifiedReachability(PPCContext& ctx, uint8_t* base) {
  const uint32_t arr = ctx.r3.u32;
  const uint64_t keep = ctx.r4.u64;
  const uint32_t data = U32(base, kObjects), num = U32(base, kObjects + 4);
  constexpr uint64_t kBits = kPendingKill | kUnreachable;
  std::vector<uint64_t> before(num, 0), host_bits(num, 0);
  for (uint32_t i = 0; i < num; ++i)
    if (const uint32_t o = U32(base, data + 4 * i)) before[i] = U64(base, o + 8) & kBits;
  g_undo = {};
  g_undo.on = true;
  const double t0 = NowMs();
  PPCContext saved = ctx;
  HostReachability(ctx, base);
  const double host_ms = NowMs() - t0;
  g_undo.on = false;
  std::vector<uint32_t> host_list(U32(base, arr + 4));
  for (uint32_t i = 0; i < host_list.size(); ++i) host_list[i] = U32(base, U32(base, arr) + 4 * i);
  const uint32_t host_count = U32(base, kMarkCount);
  // Undo: GC bits back as they were, nulled references restored (last first).
  for (uint32_t i = 0; i < num; ++i)
    if (const uint32_t o = U32(base, data + 4 * i)) {
      const uint64_t f = U64(base, o + 8);
      host_bits[i] = f & kBits;
      S64(base, o + 8, (f & ~kBits) | before[i]);
    }
  for (auto it = g_undo.dwords.rbegin(); it != g_undo.dwords.rend(); ++it) S64(base, it->first, it->second);
  for (auto it = g_undo.words.rbegin(); it != g_undo.words.rend(); ++it) S32(base, it->first, it->second);
  const double t1 = NowMs();
  ctx = saved;
  ctx.r3.u64 = arr;
  ctx.r4.u64 = keep;
  __imp__sub_825E99F0(ctx, base);
  const double guest_ms = NowMs() - t1;
  uint32_t list_diff = 0, bit_diff = 0, null_diff = 0;
  const uint32_t n = U32(base, arr + 4);
  std::vector<uint32_t> game_list(n);
  for (uint32_t i = 0; i < n; ++i) game_list[i] = U32(base, U32(base, arr) + 4 * i);
  if (ParOn()) {  // the parallel walk appends in a different order: compare as sets
    std::sort(game_list.begin(), game_list.end());
    std::sort(host_list.begin(), host_list.end());
  }
  if (n != host_list.size()) ++list_diff;
  for (uint32_t i = 0; i < n && i < host_list.size(); ++i) list_diff += game_list[i] != host_list[i];
  for (uint32_t i = 0; i < num && i < U32(base, kObjects + 4); ++i)
    if (const uint32_t o = U32(base, U32(base, kObjects) + 4 * i)) bit_diff += (U64(base, o + 8) & kBits) != host_bits[i];
  for (const auto& w : g_undo.words) null_diff += U32(base, w.first) != 0;
  REXLOG_INFO("gc: verify pass {}: host {:.1f} ms, game {:.1f} ms; list {} vs {} ({} differ), flag words differing {}, "
              "nulled refs {} ({} not nulled by the game), mark count {} vs {}{}",
              g.n + 1, host_ms, guest_ms, host_list.size(), n, list_diff, bit_diff, g_undo.words.size(), null_diff,
              host_count, U32(base, kMarkCount),
              list_diff || bit_diff || null_diff || host_count != U32(base, kMarkCount) ? " MISMATCH" : " (match)");
  g_undo = {};
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
  g.mark_ms = g.prepurge_ms = g.table_ms = g.virt_ms = 0;
  g.walk_objs = g.walk_tokens = g.walk_refs = 0;
  g.par_threads = 0;
  g.host = false;
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
  REXLOG_INFO("gc: pass {} at {:.3f} s: {:.1f} ms (purge {:.1f}, reachability {:.1f}, begin-destroy "
              "{:.1f}; host {}, table pass {:.1f}), {} objects marked, {} unreachable, {} in table, keep {:016X}, full purge {}, "
              "caller {:08X}, interval {:.1f} s{}",
              g.n, g.start / 1000.0, ms, g.prepurge_ms, g.mark_ms, ms - g.mark_ms - g.prepurge_ms, g.host ? 1 : 0, g.table_ms,
              U32(base, kMarkCount), g.unreachable, g.live_after_gc, keep, full ? 1 : 0, caller,
              engine ? F32(base, engine + kTimeBetweenPurging) : 0.0f,
              was_pending ? " (previous purge was still pending)" : "");
  if (g.host && g.par_threads)
    REXLOG_INFO("gc: par pass {}: {} threads, {} levels ({} parallel), {} virtual calls kept", g.n, g.par_threads,
                g.par_levels, g.par_parallel_levels, g.par_deferred);
  if (g.host)
    REXLOG_INFO("gc: walk pass {}: {} objects, {} tokens, {} references followed, virtual calls {:.1f} ms", g.n,
                g.walk_objs, g.walk_tokens, g.walk_refs, g.virt_ms);
}

// PerformReachabilityAnalysis(TArray<UObject*>& ObjectsToSerialize, QWORD KeepFlags)
extern "C" void sub_825E99F0(PPCContext& __restrict ctx, uint8_t* base) {
  const double t = NowMs();
  // BOTS_GC_HOST=2 (measuring): alternate the host analysis and the game's own on successive passes, so both are timed
  // under the same conditions in one run.
  g.host = HostMode() == 2 ? (g.n & 1) == 0 : HostOn();
  if (!g.host)
    __imp__sub_825E99F0(ctx, base);
  else if (VerifyOn())
    VerifiedReachability(ctx, base);
  else
    HostReachability(ctx, base);
  g.mark_ms += NowMs() - t;
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
