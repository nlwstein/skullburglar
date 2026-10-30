// The game's deferred-release queue and the second guest thread of a level load (B-288).
//
// The game keeps a double-buffered queue of objects whose release is deferred (guest 0x8382E4D0 and 0x8382E4DC: two
// TArrays of 12-byte entries {object, pointer, flags}; the word at 0x8382E470 says which of the two is the current one).
// Adding (sub_82BCD750) appends to the current array. Flushing (sub_82BCDD68, called twice in a row by several callers)
// frees the entries of the other array, flips the word and empties it. Nothing guards either: they assume one thread.
//
// But a level load runs the game's loop on a second guest thread (the loading screen; B-109 found the same for
// ProcessInput), and for a few milliseconds after `LoadChapter` both threads add and flush at once. Seen in Linux
// container runs as an intermittent SIGSEGV just after the chapter command (about 1 run in 15 under CPU load, none
// otherwise): two adders racing to grow the same array (both then write through a null data pointer), or a flush reading
// an entry whose object an adder has not stored yet (a read of guest address 8).
//
// Fix: one host lock around every add, and a flush that takes its entries out of the queue under the same lock
// (snapshot, empty, flip) and then frees them without holding it, so no guest code (the frees can wait on other
// threads) runs under the lock. The entries are freed exactly as before, in the same order.
//   BOTS_DEFERRED_QUEUE=lock    (default) the fix above
//   BOTS_DEFERRED_QUEUE=off     the game's own, unguarded code
//   BOTS_DEFERRED_QUEUE=detect  the game's own code, and a log line (first 20) each time two threads are inside the
//                               queue at once ("deferred queue: overlap"), to see how often it happens without crashing
//   BOTS_DEFERRED_QUEUE_DELAY_US=n  (test aid, with off or lock) add runs as a host copy of the game's code that sleeps n
//                               microseconds inside the unguarded steps, so the race that is a few nanoseconds wide in
//                               normal runs becomes likely at a level load: with off it crashes the way the container runs
//                               did, with lock it must not
//   BOTS_DEFERRED_QUEUE_JITTER_MS=n  (test aid) every flush first sleeps a random 0..n ms, which moves the loading-screen
//                               thread's flushes across the other thread's burst of adds

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

extern "C" void __imp__sub_82BCD750(PPCContext& __restrict ctx, uint8_t* base);  // add (object, pointer, flags)
extern "C" void __imp__sub_82BCDD68(PPCContext& __restrict ctx, uint8_t* base);  // flush
extern "C" void sub_82BCDC30(PPCContext& __restrict ctx, uint8_t* base);          // the flush's second queue
extern "C" void sub_823204C8(PPCContext& __restrict ctx, uint8_t* base);          // per entry, before its release
extern "C" void sub_82C03330(PPCContext& __restrict ctx, uint8_t* base);          // release of a flagged entry's pointer
extern "C" void sub_82350DD8(PPCContext& __restrict ctx, uint8_t* base);          // GMalloc first-use init
extern "C" void sub_82548A78(PPCContext& __restrict ctx, uint8_t* base);          // FArray::CalculateSlack(elem size)
extern "C" void sub_8234D8D8(PPCContext& __restrict ctx, uint8_t* base);          // FArray::Realloc(elem size, align)

namespace {

constexpr uint32_t kCurrent = 0x8382E470;  // 0 or 1: the array adds go to
constexpr uint32_t kArrays = 0x8382E4D0;   // two TArray headers {data, num, max}, 12 bytes each
constexpr uint32_t kGMalloc = 0x837D4F88;  // GMalloc (FMalloc*)
constexpr uint32_t kFlagFunction = 0x80000000u, kFlagKeep = 0x40000000u;

enum class Mode { kLock, kOff, kDetect };
Mode GetMode() {
  static const Mode m = [] {
    const char* v = std::getenv("BOTS_DEFERRED_QUEUE");
    const Mode mode = v && std::string(v) == "off"      ? Mode::kOff
                      : v && std::string(v) == "detect" ? Mode::kDetect
                                                        : Mode::kLock;
    REXLOG_INFO("deferred queue: {}", mode == Mode::kOff ? "off (the game's own code)" : mode == Mode::kDetect ? "detect" : "lock");
    return mode;
  }();
  return m;
}

uint8_t* Raw(uint8_t* base, uint32_t a) { return base + (uint64_t(a) + (a >= 0xE0000000u ? 0x1000u : 0u)); }
uint32_t U32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return __builtin_bswap32(v);
}
void S32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, a), &v, 4);
}

std::recursive_mutex g_lock;

// Guest GMalloc, created on first use as the game does; returns the FMalloc*.
uint32_t GMalloc(PPCContext& ctx, uint8_t* base, uint32_t lr) {
  uint32_t m = U32(base, kGMalloc);
  if (!m) {
    ctx.lr = lr;
    sub_82350DD8(ctx, base);
    m = U32(base, kGMalloc);
  }
  return m;
}
// vtable[slot](GMalloc, arg): Free is slot 3 (offset 12), the flagged-pointer release slot 5 (offset 20).
void CallGMalloc(PPCContext& ctx, uint8_t* base, uint32_t offset, uint32_t arg, uint32_t lr) {
  const uint32_t m = GMalloc(ctx, base, lr - 4);
  ctx.r3.u64 = m;
  ctx.r4.u64 = arg;
  ctx.lr = lr;
  rex::runtime::ResolveIndirectFunction(U32(base, U32(base, m) + offset))(ctx, base);
}

struct Entry {
  uint32_t object, pointer, flags;
};

// Two threads inside the queue at once (BOTS_DEFERRED_QUEUE=detect).
std::atomic<int> g_in_add{0}, g_in_flush{0};
std::atomic<uint32_t> g_overlaps{0};
void NoteOverlap(const char* what) {
  const uint32_t n = ++g_overlaps;
  if (n <= 20) REXLOG_WARN("deferred queue: overlap #{}: {}", n, what);
}

int TestDelayUs() {
  static const int v = [] {
    const char* e = std::getenv("BOTS_DEFERRED_QUEUE_DELAY_US");
    const int us = e && *e ? std::atoi(e) : 0;
    if (us) REXLOG_WARN("deferred queue: test delay of {} us inside the add (BOTS_DEFERRED_QUEUE_DELAY_US)", us);
    return us;
  }();
  return v;
}
int TestJitterMs() {
  static const int v = [] {
    const char* e = std::getenv("BOTS_DEFERRED_QUEUE_JITTER_MS");
    return e && *e ? std::atoi(e) : 0;
  }();
  return v;
}
void TestJitter() {
  if (const int ms = TestJitterMs(); ms > 0) {
    static thread_local uint32_t state = 2463534242u ^ uint32_t(reinterpret_cast<uintptr_t>(&state));
    state ^= state << 13, state ^= state >> 17, state ^= state << 5;
    std::this_thread::sleep_for(std::chrono::microseconds(state % (uint32_t(ms) * 1000u)));
  }
}
void TestDelay() { std::this_thread::sleep_for(std::chrono::microseconds(TestDelayUs())); }

// The game's add, step for step (sub_82BCD750), with TestDelay() between the steps another thread could see.
void AddWithDelay(PPCContext& ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32, pointer = ctx.r4.u32, flag_function = ctx.r5.u32, flag_keep = ctx.r6.u32;
  const uint32_t array = kArrays + 12 * (U32(base, kCurrent) & 1);
  const uint32_t n = U32(base, array + 4), max = U32(base, array + 8);
  S32(base, array + 4, n + 1);
  TestDelay();
  if (int32_t(n + 1) > int32_t(max)) {
    ctx.r3.u64 = array;
    ctx.r4.u64 = 12;
    ctx.lr = 0x82BCD7B0;
    sub_82548A78(ctx, base);
    S32(base, array + 8, ctx.r3.u32);
    TestDelay();
    ctx.r5.u64 = 8;
    ctx.r4.u64 = 12;
    ctx.r3.u64 = array;
    ctx.lr = 0x82BCD7C4;
    sub_8234D8D8(ctx, base);  // FArray::Realloc(array, element size, alignment)
  }
  TestDelay();
  const uint32_t entry = U32(base, array) + 12 * n;
  S32(base, entry, object);
  S32(base, U32(base, array) + 12 * n + 4, pointer);
  S32(base, U32(base, array) + 12 * n + 8,
      (U32(base, U32(base, array) + 12 * n + 8) & 0x7FFFFFFFu) | ((flag_function & 1) << 31));
  S32(base, U32(base, array) + 12 * n + 8,
      (U32(base, U32(base, array) + 12 * n + 8) & 0xBFFFFFFFu) | ((flag_keep & 1) << 30));
}

}  // namespace

extern "C" void sub_82BCD750(PPCContext& __restrict ctx, uint8_t* base) {
  switch (GetMode()) {
    case Mode::kLock: {
      std::lock_guard lock(g_lock);
      if (TestDelayUs()) AddWithDelay(ctx, base);
      else __imp__sub_82BCD750(ctx, base);
      return;
    }
    case Mode::kDetect: {
      if (const uint32_t n = U32(base, kArrays + 12 * (U32(base, kCurrent) & 1) + 4); n % 256 == 255) {
        static std::atomic<int> logged{0};
        if (logged++ < 40) REXLOG_INFO("deferred queue: add: current array holds {} entries", n + 1);
      }
      if (g_in_add.fetch_add(1) > 0) NoteOverlap("two adds at once");
      if (g_in_flush.load() > 0) NoteOverlap("add during a flush");
      __imp__sub_82BCD750(ctx, base);
      g_in_add.fetch_sub(1);
      return;
    }
    case Mode::kOff:
      if (TestDelayUs()) AddWithDelay(ctx, base);
      else __imp__sub_82BCD750(ctx, base);
      return;
  }
}

extern "C" void sub_82BCDD68(PPCContext& __restrict ctx, uint8_t* base) {
  const Mode mode = GetMode();
  TestJitter();
  if (mode != Mode::kLock) {
    if (mode == Mode::kDetect) {
      static std::atomic<int> logged{0};
      const uint32_t n0 = U32(base, kArrays + 4), n1 = U32(base, kArrays + 12 + 4);
      if ((n0 || n1) && logged++ < 40)
        REXLOG_INFO("deferred queue: flush: current array {}, entries {} / {}", U32(base, kCurrent), n0, n1);
      if (g_in_flush.fetch_add(1) > 0) NoteOverlap("two flushes at once");
      if (g_in_add.load() > 0) NoteOverlap("flush during an add");
    }
    __imp__sub_82BCDD68(ctx, base);
    if (mode == Mode::kDetect) g_in_flush.fetch_sub(1);
    return;
  }

  // Take the entries of the non-current array out under the lock, empty it and make it the current one.
  std::vector<Entry> entries;
  {
    std::lock_guard lock(g_lock);
    const uint32_t current = U32(base, kCurrent) & 1;
    const uint32_t array = kArrays + 12 * (1 - current);
    const uint32_t data = U32(base, array), num = U32(base, array + 4);
    if (data && int32_t(num) > 0) {
      entries.reserve(num);
      for (uint32_t i = 0; i < num; ++i) {
        const uint32_t e = data + 12 * i;
        entries.push_back({U32(base, e), U32(base, e + 4), U32(base, e + 8)});
      }
    }
    S32(base, kCurrent, 1 - current);
    S32(base, array + 4, 0);
  }
  // Release them as the game's loop does, without the lock.
  for (const Entry& e : entries) {
    ctx.r3.u64 = e.object;
    ctx.lr = 0x82BCDDC0;
    sub_823204C8(ctx, base);
    if (e.flags & kFlagFunction) {
      ctx.r3.u64 = e.pointer;
      ctx.lr = 0x82BCDDD8;
      sub_82C03330(ctx, base);
    } else if (!(e.flags & kFlagKeep)) {
      CallGMalloc(ctx, base, 20, e.pointer, 0x82BCDE14);
    }
    CallGMalloc(ctx, base, 12, e.object, 0x82BCDE40);
  }
  ctx.lr = 0x82BCDE6C;
  sub_82BCDC30(ctx, base);
}
