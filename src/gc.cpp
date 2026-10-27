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
// Almost all of a pass is the reachability analysis, and almost all of that is the reference-token walk: for each
// reachable object, read its body and the flags of every object it references. Each of those is a cache miss on a
// laptop (the objects are separate heap blocks, ~25-60k of them reachable). The Xbox 360 code prefetches the next
// object (dcbt), which the recompiler drops.
//
// Host analysis (default on, BOTS_GC_HOST=0 turns it off): sub_825E99F0 is replaced by a host implementation of the
// same algorithm (see HostReachability) that prefetches ahead in stages: the headers of the objects 8 ahead, the lines
// holding the references of the object 4 ahead, the flag words those references point to 2 ahead, and the flag words
// of array elements 1 ahead. What GC marks, keeps, nulls and frees is unchanged.
//   BOTS_GC_HOST=0     the game's own code
//   BOTS_GC_HOST=2     (measuring) alternate the two on successive passes
//   BOTS_GC_HELPER=N   a helper thread loads what the walk will read, N objects ahead (default 24; 0: no helper)
//   BOTS_GC_PF_HEAD / _FIELDS / _REFS / _ELEMS   the look-ahead distances (8 / 4 / 2 / 1; 0 turns a stage off)
//   BOTS_GC_VERIFY=1   (with the host analysis) run both on every pass and log whether they agree ("gc: verify")
//
// Log (BOTS_GC_LOG, default 1; 0 silences it): one line per CollectGarbage ("gc: pass ..."; times, objects in the
// table, objects found unreachable), one when the purge that follows completes ("gc: purged ..."; live objects
// before and after), and one per game frame longer than BOTS_GC_SLOW_MS (default 50) with the GC time inside that
// frame ("gc: slow frame ..."), so long frames can be told apart into GC and not GC.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <utility>
#include <vector>

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
  double table_ms = 0;
  bool host = false;          // this pass used the host analysis
  uint64_t walk_objs = 0, walk_tokens = 0, walk_refs = 0;  // host walk counts
  double virt_ms = 0;         // BOTS_GC_PROF=1: time in the per-object virtual (vtable +244)
  double replay_ms[3] = {};   // BOTS_GC_PROF=1: a read-only replay of the walk's loads over the final list, twice        // host analysis: the pass over the object table (the rest of mark_ms is the walk)
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

// ---- Host PerformReachabilityAnalysis --------------------------------------------------------------------------
// The same algorithm as sub_825E99F0, instruction for instruction in effect: the same loads and stores of guest memory
// in the same order, the same guest calls (virtuals, TArray growth through the game's allocator, AddItem) with the
// same arguments. What differs is only how the host gets there: a native token interpreter instead of the
// recompiled one, and prefetches. Object i+2's first 512 bytes are prefetched while object i is processed (the
// Xbox 360 code prefetches object i+1 the same way, with dcbt, which the recompiler drops), and object i+1's
// top-level references are looked up ahead of time so the flag words they point to are already on their way.

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

// Read-only look-ahead over object `obj`'s top-level reference tokens, in stages that each need the previous one's
// lines in the cache: kFields prefetches the lines of the object that hold its references; kRefs reads those
// references and prefetches the flag words they point to (and the data of its TArray<UObject*> fields); kElems
// prefetches the flag words of those arrays' elements. Tokens of arrays of structs (type 4) are skipped; tokens of
// fixed arrays (type 5) address the first element, which is part of the object.
enum Stage { kFieldLines, kRefFlags, kElemFlags };
void PrefetchStage(uint8_t* base, uint32_t obj, Stage stage) {
  const uint32_t cls = U32(base, obj + 52);
  if (!cls) return;
  const uint32_t tokens = U32(base, cls + 480), count = U32(base, cls + 484);
  if (!tokens) return;
  const uint32_t pool0 = U32(base, kPoolStart), pool1 = U32(base, kPoolEnd);
  auto flags = [&](uint32_t r) {
    if (r && (r < pool0 || r > pool1)) Prefetch(base, r + 8);
  };
  for (uint32_t i = 0, steps = 0; i < count && steps < 512; ++steps) {
    const uint32_t t = U32(base, tokens + 4 * i++);
    const uint32_t type = t >> 16 & 0xFF, off = t & 0xFFFF;
    if (type == 6) return;
    if (type == 4) {
      if (i + 1 >= count) return;
      const uint32_t target = (U32(base, tokens + 4 * (i + 1)) + i + 1) & 0xFFFFFF;
      if (target <= i || target > count) return;
      i = target;
      continue;
    }
    if (type == 5) {
      i += 2;
      continue;
    }
    if (type != 1 && type != 2 && type != 3 && type != 7) continue;
    if (stage == kFieldLines) {
      Prefetch(base, obj + off);
    } else if (type != 3) {
      if (stage == kRefFlags) flags(U32(base, obj + off));
    } else if (const uint32_t data = U32(base, obj + off)) {
      if (stage == kRefFlags) {
        Prefetch(base, data);
      } else {
        const uint32_t n = U32(base, obj + off + 4);
        for (uint32_t j = 0; j < n && j < 16; ++j) flags(U32(base, data + 4 * j));
      }
    }
  }
}

// ---- Warming helper thread ------------------------------------------------------------------------------------
// One core can only have about ten cache misses in flight, and the walk already keeps it near that. A second thread
// that walks the same objects a little ahead and actually loads what the walk will read (reference fields, the flag
// words they point to, array elements' flags) brings those lines into the shared L3 with its own core's misses, so
// the walk finds them there. The helper only reads guest memory, and only objects the walk handed it (entries of
// ObjectsToSerialize, which stay valid for the whole analysis since nothing is freed until the purge); it never
// writes guest memory, so what GC does is unchanged. Without it (BOTS_GC_HELPER=0) the walk runs alone.
struct Warmer {
  static constexpr uint32_t kRing = 1024;
  std::atomic<uint32_t> ring[kRing] = {};
  std::atomic<uint32_t> head{0};    // written by the walk
  std::atomic<bool> active{false};  // between Start and Stop
  std::atomic<uint64_t> warmed{0};
  uint8_t* base = nullptr;
  std::mutex m;
  std::condition_variable cv;
  bool started = false;

  void Push(uint32_t obj) {
    const uint32_t h = head.load(std::memory_order_relaxed);
    ring[h % kRing].store(obj, std::memory_order_relaxed);
    head.store(h + 1, std::memory_order_release);
  }
  void Start(uint8_t* b) {
    if (!started) {
      started = true;
      base = b;
      std::thread([this] { Run(); }).detach();
    }
    head.store(0, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(m);
      active.store(true, std::memory_order_release);
    }
    cv.notify_one();
  }
  void Stop() { active.store(false, std::memory_order_release); }

  void Run() {
    volatile uint64_t sink = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this] { return active.load(std::memory_order_acquire); });
      }
      uint32_t tail = 0;
      uint64_t n = 0;
      while (active.load(std::memory_order_acquire)) {
        const uint32_t h = head.load(std::memory_order_acquire);
        if (h == tail || h < tail) {
          if (h < tail) tail = h;  // a new analysis started
#if defined(__x86_64__) || defined(_M_X64)
          __builtin_ia32_pause();
#endif
          continue;
        }
        if (h - tail > kRing / 2) tail = h - kRing / 2;  // fell behind: skip to recent work
        const uint32_t obj = ring[tail % kRing].load(std::memory_order_relaxed);
        ++tail;
        if (obj) sink += Warm(obj), ++n;
      }
      warmed.fetch_add(n, std::memory_order_relaxed);
    }
  }

  // Load what the walk will load for `obj` (top-level reference tokens; see PrefetchStage).
  uint64_t Warm(uint32_t obj) {
    uint64_t acc = 0;
    const uint32_t cls = U32(base, obj + 52);
    if (!cls) return 0;
    const uint32_t tokens = U32(base, cls + 480), count = U32(base, cls + 484);
    if (!tokens) return 0;
    const uint32_t pool0 = U32(base, kPoolStart), pool1 = U32(base, kPoolEnd);
    auto flags = [&](uint32_t r) {
      if (r && (r < pool0 || r > pool1)) acc += U32(base, r + 8);
    };
    for (uint32_t i = 0, steps = 0; i < count && steps < 512; ++steps) {
      const uint32_t t = U32(base, tokens + 4 * i++);
      const uint32_t type = t >> 16 & 0xFF, off = t & 0xFFFF;
      if (type == 6) break;
      if (type == 4) {
        if (i + 1 >= count) break;
        const uint32_t target = (U32(base, tokens + 4 * (i + 1)) + i + 1) & 0xFFFFFF;
        if (target <= i || target > count) break;
        i = target;
        continue;
      }
      if (type == 5) {
        i += 2;
        continue;
      }
      if (type == 1 || type == 2 || type == 7) {
        flags(U32(base, obj + off));
      } else if (type == 3) {
        const uint32_t data = U32(base, obj + off), n = U32(base, obj + off + 4);
        for (uint32_t j = 0; data && j < n && j < 64; ++j) flags(U32(base, data + 4 * j));
      }
    }
    return acc;
  }
};
Warmer& TheWarmer() {
  static Warmer* w = new Warmer();  // never destroyed: its thread lives for the process
  return *w;
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

  // Look-ahead distances in objects (0 turns a stage off).
  static const uint32_t kHead = uint32_t(EnvInt("BOTS_GC_PF_HEAD", 8)), kFields = uint32_t(EnvInt("BOTS_GC_PF_FIELDS", 4)),
                        kRefs = uint32_t(EnvInt("BOTS_GC_PF_REFS", 2)), kElems = uint32_t(EnvInt("BOTS_GC_PF_ELEMS", 1));
  // Pass 2: the reference-token walk. Every object reached is cleared of RF_Unreachable and appended.
  const uint32_t pool0 = U32(base, kPoolStart), pool1 = U32(base, kPoolEnd);
  // GPermanentObjectPool bounds are re-read per reference by the original; nothing in the walk can change them.
  auto in_pool = [&](uint32_t r) { return !(r < pool0) && r <= pool1; };
  static const uint32_t kHelperAhead = uint32_t(EnvInt("BOTS_GC_HELPER", 24));  // 0: no helper thread
  Warmer* warmer = kHelperAhead ? &TheWarmer() : nullptr;
  uint32_t pushed = 0;  // ObjectsToSerialize entries handed to the helper so far
  if (warmer) warmer->Start(base);
  if (int32_t(U32(base, arr + 4)) > 0) {
    uint32_t next = 0;  // index of the next object to serialize
    for (;;) {
      const uint32_t data = U32(base, arr);
      const uint32_t obj = U32(base, data + 4 * next);
      ++next;
      const uint32_t count_now = U32(base, arr + 4);
      // A software pipeline over the objects ahead (see PrefetchStage): headers kHead ahead, the lines holding their
      // references kFields ahead, the referenced objects' flags kRefs ahead, array elements' flags kElems ahead.
      const uint32_t i = next - 1;
      auto ahead = [&](uint32_t d) { return d && i + d < count_now ? U32(base, data + 4 * (i + d)) : 0u; };
      if (const uint32_t o = ahead(kHead)) {
        Prefetch(base, o);
        Prefetch(base, o + 52);
      }
      if (const uint32_t o = ahead(kFields)) PrefetchStage(base, o, kFieldLines);
      if (const uint32_t o = ahead(kRefs)) PrefetchStage(base, o, kRefFlags);
      if (const uint32_t o = ahead(kElems)) PrefetchStage(base, o, kElemFlags);
      if (warmer)
        for (const uint32_t until = std::min(count_now, i + kHelperAhead); pushed < until; ++pushed)
          warmer->Push(U32(base, data + 4 * pushed));
      ctx.r3.u64 = obj;
      ctx.r4.u64 = arr;
      static const bool kProf = EnvInt("BOTS_GC_PROF", 0) != 0;
      const double tv = kProf ? NowMs() : 0;
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
  if (warmer) warmer->Stop();
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
  if (n != host_list.size()) ++list_diff;
  for (uint32_t i = 0; i < n && i < host_list.size(); ++i) list_diff += U32(base, U32(base, arr) + 4 * i) != host_list[i];
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
  g.replay_ms[0] = g.replay_ms[1] = g.replay_ms[2] = 0;
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
  if (g.host)
    REXLOG_INFO("gc: walk pass {}: {} objects, {} tokens, {} references followed, virtual calls {:.1f} ms, "
                "read-only replay {:.1f} ms, again {:.1f} ms, again with prefetch {:.1f} ms", g.n, g.walk_objs,
                g.walk_tokens, g.walk_refs, g.virt_ms, g.replay_ms[0], g.replay_ms[1], g.replay_ms[2]);
}

// PerformReachabilityAnalysis(TArray<UObject*>& ObjectsToSerialize, QWORD KeepFlags)
extern "C" void sub_825E99F0(PPCContext& __restrict ctx, uint8_t* base) {
  const double t = NowMs();
  const uint32_t arr = ctx.r3.u32;
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
  static const bool kProf = EnvInt("BOTS_GC_PROF", 0) != 0;
  if (kProf && g.host) {
    // How much of the walk is the memory it reads: replay its loads (fields, referenced flags) over the final
    // ObjectsToSerialize, read-only, once cold and once warm.
    Warmer& w = TheWarmer();
    w.base = base;
    volatile uint64_t sink = 0;
    for (int k = 0; k < 3; ++k) {
      const double t0 = NowMs();
      const uint32_t data = U32(base, arr), n = U32(base, arr + 4);
      for (uint32_t i = 0; i < n; ++i) {
        if (k == 2) {  // the third time with the walk's prefetch pipeline
          auto at = [&](uint32_t d) { return i + d < n ? U32(base, data + 4 * (i + d)) : 0u; };
          if (const uint32_t o = at(8)) Prefetch(base, o), Prefetch(base, o + 52);
          if (const uint32_t o = at(4)) PrefetchStage(base, o, kFieldLines);
          if (const uint32_t o = at(2)) PrefetchStage(base, o, kRefFlags);
          if (const uint32_t o = at(1)) PrefetchStage(base, o, kElemFlags);
        }
        sink += w.Warm(U32(base, data + 4 * i));
      }
      g.replay_ms[k] = NowMs() - t0;
    }
  }
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
