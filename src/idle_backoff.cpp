// Idle back-off for guest threads that poll with appSleep(0).
//
//   sub_82474678  appSleep(float Seconds): SleepEx(Seconds * 1000, FALSE) through sub_82448970, which is
//                 KeDelayExecutionThread. With 0 seconds the runtime yields (SwitchToThread / sched_yield) and returns.
//
// UE3's rendering thread loop (sub_825698C8, RenderingThreadMain) calls appSleep(0) whenever the command ring buffer is
// empty and loops. On the Xbox 360 that thread had a hardware thread to itself; here it is an OS thread that keeps a
// logical CPU 100% busy while the renderer waits for the game thread (measured on a 4-core, 15 W laptop: the render
// thread used 94% of a CPU, 62% of its wall time inside this yield loop). On a power-limited machine that CPU time comes
// out of the shared package budget, so the game thread and the integrated GPU clock lower.
//
// The back-off: once a thread has called appSleep(0) BOTS_IDLE_SPINS times in a row with no real work in between
// (each call less than 50 us after the previous one returned), further zero sleeps sleep for BOTS_IDLE_SLEEP_US
// microseconds (default 200) instead of yielding. Any longer gap (the thread found work) resets the count.
// Effect on the game: appSleep(0) only gives up the CPU; it has no result and changes no state, so the only change is
// that a thread polling an empty queue notices new work up to BOTS_IDLE_SLEEP_US later. The game thread's wait for the
// rendering thread (one fence per frame) can therefore take up to that much longer; nothing else observes it.
// BOTS_IDLE_SPINS=0 turns the back-off off (default 64). Every 10 s it logs how many zero sleeps it saw and slept.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <rex/logging.h>
#include <rex/ppc/context.h>

extern "C" void __imp__sub_82474678(PPCContext& __restrict ctx, uint8_t* base);

namespace {
using clk = std::chrono::steady_clock;

struct Config {
  int spins = 64;
  int sleep_us = 200;
  Config() {
    if (const char* v = std::getenv("BOTS_IDLE_SPINS")) spins = std::atoi(v);
    if (const char* v = std::getenv("BOTS_IDLE_SLEEP_US")) sleep_us = std::max(1, std::atoi(v));
    REXLOG_INFO("idle: appSleep(0) back-off {} (after {} spins, {} us)", spins > 0 ? "on" : "off", spins, sleep_us);
  }
};
const Config& Cfg() {
  static Config c;
  return c;
}

std::atomic<uint64_t> g_zero{0}, g_slept{0};

void ShortSleep(int us) {
#if defined(_WIN32)
  // Sleep() rounds up to the system timer period (1 ms at best); a high-resolution waitable timer (Windows 10 1803+)
  // sleeps for about the time asked.
  thread_local HANDLE timer = [] {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,
                                      TIMER_ALL_ACCESS);
    return h ? h : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }();
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(us) * 10;  // relative, 100 ns units
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}
}  // namespace

extern "C" void sub_82474678(PPCContext& __restrict ctx, uint8_t* base) {
  const Config& cfg = Cfg();
  if (cfg.spins <= 0 || ctx.f1.f64 != 0.0) return __imp__sub_82474678(ctx, base);
  thread_local clk::time_point last_return{};
  thread_local int streak = 0;
  const auto now = clk::now();
  streak = now - last_return < std::chrono::microseconds(50) ? streak + 1 : 0;
  g_zero.fetch_add(1, std::memory_order_relaxed);
  if (streak >= cfg.spins) {
    ShortSleep(cfg.sleep_us);
    g_slept.fetch_add(1, std::memory_order_relaxed);
  } else {
    __imp__sub_82474678(ctx, base);
  }
  last_return = clk::now();
  static std::atomic<int64_t> next_log{0};
  const int64_t s = std::chrono::duration_cast<std::chrono::seconds>(last_return.time_since_epoch()).count();
  int64_t due = next_log.load(std::memory_order_relaxed);
  if (s >= due && next_log.compare_exchange_strong(due, s + 10)) {
    if (due) REXLOG_INFO("idle: {} appSleep(0) calls, {} backed off", g_zero.exchange(0), g_slept.exchange(0));
  }
}
