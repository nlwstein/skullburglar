// Slow-frame sampler, BOTS_SLOW_SAMPLE=<ms> (macOS and Windows): stacks of the game thread, taken only while one game frame has
// run longer than <ms>. A frame is the time between two calls of the viewport's input poll (Tick() below, called from
// the game_hooks.cpp poll hook), which the game thread makes once per engine tick.
//
// A sampler thread wakes BOTS_SLOW_SAMPLE_HZ times a second (default 2000). While the current frame is older than the
// threshold it suspends the game thread, walks its frame-pointer chain, and resumes it. When the frame ends, the
// frame's samples are appended to BOTS_SLOW_SAMPLE_FILE (default slow_frames.txt) as
//   "# frame <seconds since start> <frame ms> samples=<n>"
// then folded stacks "<root>;...;<leaf> <count>", frames as <module>+0x<offset>, and one log line is written
// ("slow frame: ..."). Samples are wall clock: a wait shows up as the wait's stack. Resolve offline with
// atos -o <module> -l <load address> (the executable loads at 0x100000000, dylibs at 0).
// Nothing allocates while the game thread is suspended (it could hold the heap lock).
// Windows (x64): the same, but the code has no frame pointers, so a sample is the instruction pointer plus the return
// addresses found by scanning the top of the stack (a word inside an image whose preceding bytes are a call). Frames
// are <module>+0x<offset> (resolve the executable's with llvm-symbolizer and its PDB); stale words can add a frame.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <rex/logging.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>

#include <map>
#include <unordered_map>
#include <vector>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <map>
#include <unordered_map>
#include <vector>
#endif

namespace bots::slowframe {

#if defined(__APPLE__) && defined(__aarch64__)
namespace {

using clk = std::chrono::steady_clock;

std::atomic<int64_t> g_frame_start_us{0};  // start of the current game frame (steady clock, us)
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_started{false};
thread_act_t g_thread = MACH_PORT_NULL;
uintptr_t g_stack_lo = 0, g_stack_hi = 0;
const clk::time_point g_t0 = clk::now();

int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - g_t0).count(); }

constexpr int kMaxDepth = 192;
constexpr uintptr_t kAddrMask = 0x00007FFFFFFFFFFFull;  // strips pointer-authentication bits

// One stack, leaf first. Called with the thread suspended: reads only memory inside the thread's stack.
int Walk(uintptr_t* out) {
  arm_thread_state64_t st;
  mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
  if (thread_get_state(g_thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&st), &n) != KERN_SUCCESS)
    return 0;
  int d = 0;
  out[d++] = uintptr_t(arm_thread_state64_get_pc(st)) & kAddrMask;
  uintptr_t fp = uintptr_t(arm_thread_state64_get_fp(st)) & kAddrMask;
  while (d < kMaxDepth && fp >= g_stack_lo && fp + 16 <= g_stack_hi && (fp & 7) == 0) {
    const uintptr_t* f = reinterpret_cast<const uintptr_t*>(fp);
    const uintptr_t next = f[0] & kAddrMask, ret = f[1] & kAddrMask;
    if (!ret) break;
    out[d++] = ret - 1;  // inside the call instruction, for the symbolizer
    if (next <= fp) break;
    fp = next;
  }
  return d;
}

std::string Frame(uintptr_t a, std::unordered_map<uintptr_t, std::string>& cache) {
  auto it = cache.find(a);
  if (it != cache.end()) return it->second;
  Dl_info info{};
  char buf[160];
  if (dladdr(reinterpret_cast<void*>(a), &info) && info.dli_fname) {
    const char* base = std::strrchr(info.dli_fname, '/');
    std::snprintf(buf, sizeof buf, "%s+0x%lx", base ? base + 1 : info.dli_fname,
                  static_cast<unsigned long>(a - reinterpret_cast<uintptr_t>(info.dli_fbase)));
  } else {
    std::snprintf(buf, sizeof buf, "?+0x%lx", static_cast<unsigned long>(a));
  }
  return cache.emplace(a, buf).first->second;
}

void Run(double threshold_ms, int hz) {
  const char* path = std::getenv("BOTS_SLOW_SAMPLE_FILE");
  FILE* out = std::fopen(path && *path ? path : "slow_frames.txt", "w");
  if (!out) {
    REXLOG_WARN("slow frame sampler: can't open its output file");
    return;
  }
  const int64_t threshold_us = int64_t(threshold_ms * 1000);
  const auto period = std::chrono::microseconds(1000000 / hz);
  std::vector<uintptr_t> buf;  // stacks of the current slow frame: depth, then the frames
  buf.reserve(1 << 20);
  std::unordered_map<uintptr_t, std::string> names;
  uint64_t frame_id = 0;
  int64_t frame_start = 0;
  int samples = 0;
  uintptr_t stack[kMaxDepth];
  auto flush = [&](int64_t end_us) {
    std::map<std::string, int> folded;
    for (size_t i = 0; i < buf.size();) {
      const size_t d = buf[i++];
      std::string key;
      for (size_t j = d; j-- > 0;) {
        if (!key.empty()) key += ';';
        key += Frame(buf[i + j], names);
      }
      folded[key]++;
      i += d;
    }
    const double ms = (end_us - frame_start) / 1000.0;
    std::fprintf(out, "# frame %.3f %.1f samples=%d\n", frame_start / 1e6, ms, samples);
    for (auto& [k, c] : folded) std::fprintf(out, "%s %d\n", k.c_str(), c);
    std::fflush(out);
    REXLOG_INFO("slow frame: {:.1f} ms at {:.3f} s, {} samples (BOTS_SLOW_SAMPLE_FILE)", ms, frame_start / 1e6,
                samples);
    buf.clear();
    samples = 0;
  };
  for (;;) {
    std::this_thread::sleep_for(period);
    const uint64_t id = g_frames.load(std::memory_order_acquire);
    const int64_t start = g_frame_start_us.load(std::memory_order_acquire);
    if (samples && id != frame_id) flush(start);
    if (id != frame_id) frame_id = id, frame_start = start;
    if (NowUs() - start < threshold_us) continue;
    if (thread_suspend(g_thread) != KERN_SUCCESS) continue;
    const int d = Walk(stack);
    thread_resume(g_thread);
    if (g_frames.load(std::memory_order_acquire) != frame_id || d == 0) continue;  // the frame ended meanwhile
    buf.push_back(uintptr_t(d));
    buf.insert(buf.end(), stack, stack + d);
    ++samples;
  }
}

}  // namespace

void Tick() {
  static const double threshold = [] {
    const char* v = std::getenv("BOTS_SLOW_SAMPLE");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (threshold <= 0) return;
  g_frame_start_us.store(NowUs(), std::memory_order_release);
  g_frames.fetch_add(1, std::memory_order_acq_rel);
  if (g_started.exchange(true)) return;
  pthread_t self = pthread_self();
  g_thread = pthread_mach_thread_np(self);
  g_stack_hi = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
  g_stack_lo = g_stack_hi - pthread_get_stacksize_np(self);
  const char* hzv = std::getenv("BOTS_SLOW_SAMPLE_HZ");
  const int hz = hzv && std::atoi(hzv) > 0 ? std::atoi(hzv) : 2000;
  REXLOG_INFO("slow frame sampler: frames over {} ms, {} Hz", threshold, hz);
  std::thread(Run, threshold, hz).detach();
}
#elif defined(_WIN32)
namespace {

using clk = std::chrono::steady_clock;

std::atomic<int64_t> g_frame_start_us{0};
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_started{false};
HANDLE g_thread = nullptr;
uintptr_t g_stack_lo = 0, g_stack_hi = 0;
const clk::time_point g_t0 = clk::now();

int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - g_t0).count(); }

constexpr int kMaxDepth = 40;
constexpr int kScanWords = 2048;  // the top 16 KB of the stack

bool InImage(uintptr_t a) {
  static const uintptr_t lo = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  static const uintptr_t hi = lo + reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                                       lo + reinterpret_cast<const IMAGE_DOS_HEADER*>(lo)->e_lfanew)
                                       ->OptionalHeader.SizeOfImage;
  return a >= lo + 16 && a < hi;
}
// A return address: the bytes before it end a call instruction (E8 rel32, or FF /2 in its 2, 3, 6 or 7 byte forms).
bool AfterCall(uintptr_t a) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
  return p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
         (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
}

// One stack, leaf first. Called with the thread suspended: reads only the thread's stack and the image.
int Walk(uintptr_t* out) {
  CONTEXT ctx;
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (!GetThreadContext(g_thread, &ctx)) return 0;
  int d = 0;
  out[d++] = uintptr_t(ctx.Rip);
  const uintptr_t sp = uintptr_t(ctx.Rsp) & ~uintptr_t(7);
  for (int i = 0; i < kScanWords && d < kMaxDepth; ++i) {
    const uintptr_t at = sp + 8 * uintptr_t(i);
    if (at < g_stack_lo || at + 8 > g_stack_hi) break;
    const uintptr_t v = *reinterpret_cast<const uintptr_t*>(at);
    if (InImage(v) && AfterCall(v)) out[d++] = v - 1;
  }
  return d;
}

std::string Frame(uintptr_t a, std::unordered_map<uintptr_t, std::string>& cache) {
  auto it = cache.find(a);
  if (it != cache.end()) return it->second;
  HMODULE mod = nullptr;
  char buf[200];
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(a), &mod) &&
      mod) {
    char name[MAX_PATH] = {};
    GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = std::strrchr(name, '\\');
    std::snprintf(buf, sizeof buf, "%s+0x%llx", base ? base + 1 : name,
                  static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(mod)));
  } else {
    std::snprintf(buf, sizeof buf, "?+0x%llx", static_cast<unsigned long long>(a));
  }
  return cache.emplace(a, buf).first->second;
}

void Run(double threshold_ms, int hz) {
  const char* path = std::getenv("BOTS_SLOW_SAMPLE_FILE");
  FILE* out = std::fopen(path && *path ? path : "slow_frames.txt", "w");
  if (!out) {
    REXLOG_WARN("slow frame sampler: can't open its output file");
    return;
  }
  const int64_t threshold_us = int64_t(threshold_ms * 1000);
  const auto period = std::chrono::microseconds(1000000 / hz);
  std::vector<uintptr_t> buf;  // stacks of the current slow frame: depth, then the frames
  buf.reserve(1 << 20);
  std::unordered_map<uintptr_t, std::string> names;
  uint64_t frame_id = 0;
  int64_t frame_start = 0;
  int samples = 0;
  uintptr_t stack[kMaxDepth];
  auto flush = [&](int64_t end_us) {
    std::map<std::string, int> folded;
    for (size_t i = 0; i < buf.size();) {
      const size_t d = buf[i++];
      std::string key;
      for (size_t j = d; j-- > 0;) {
        if (!key.empty()) key += ';';
        key += Frame(buf[i + j], names);
      }
      folded[key]++;
      i += d;
    }
    const double ms = (end_us - frame_start) / 1000.0;
    std::fprintf(out, "# frame %.3f %.1f samples=%d\n", frame_start / 1e6, ms, samples);
    for (auto& [k, c] : folded) std::fprintf(out, "%s %d\n", k.c_str(), c);
    std::fflush(out);
    REXLOG_INFO("slow frame: {:.1f} ms at {:.3f} s, {} samples (BOTS_SLOW_SAMPLE_FILE)", ms, frame_start / 1e6,
                samples);
    buf.clear();
    samples = 0;
  };
  for (;;) {
    std::this_thread::sleep_for(period);
    const uint64_t id = g_frames.load(std::memory_order_acquire);
    const int64_t start = g_frame_start_us.load(std::memory_order_acquire);
    if (samples && id != frame_id) flush(start);
    if (id != frame_id) frame_id = id, frame_start = start;
    if (NowUs() - start < threshold_us) continue;
    if (SuspendThread(g_thread) == DWORD(-1)) continue;
    const int d = Walk(stack);
    ResumeThread(g_thread);
    if (g_frames.load(std::memory_order_acquire) != frame_id || d == 0) continue;  // the frame ended meanwhile
    buf.push_back(uintptr_t(d));
    buf.insert(buf.end(), stack, stack + d);
    ++samples;
  }
}

}  // namespace

void Tick() {
  static const double threshold = [] {
    const char* v = std::getenv("BOTS_SLOW_SAMPLE");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (threshold <= 0) return;
  g_frame_start_us.store(NowUs(), std::memory_order_release);
  g_frames.fetch_add(1, std::memory_order_acq_rel);
  if (g_started.exchange(true)) return;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
                       THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
    return;
  ULONG_PTR lo, hi;
  GetCurrentThreadStackLimits(&lo, &hi);
  g_stack_lo = lo, g_stack_hi = hi;
  const char* hzv = std::getenv("BOTS_SLOW_SAMPLE_HZ");
  const int hz = hzv && std::atoi(hzv) > 0 ? std::atoi(hzv) : 1000;
  REXLOG_INFO("slow frame sampler: frames over {} ms, {} Hz", threshold, hz);
  std::thread(Run, threshold, hz).detach();
}
#else
void Tick() {}
#endif

}  // namespace bots::slowframe
