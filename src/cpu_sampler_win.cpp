// Windows helpers for performance work, where no profiler is installed on the test machine:
//
//  - SampleThreadCpuWin(): per-thread CPU seconds and names for BOTS_THREAD_STATS (thread_stats.h).
//  - A wall-clock stack sampler, BOTS_CPU_SAMPLE=<names>: a thread that, BOTS_CPU_SAMPLE_HZ times a second (default
//    500), suspends each selected thread, unwinds its stack (RtlVirtualUnwind over the modules' .pdata) and resumes
//    it. Waits are sampled too (the stack then ends in ntdll), so the result says where a thread's wall time goes,
//    not only its CPU time. <names> is a comma-separated list of substrings of thread descriptions (for example
//    "XThread"), or "hot:<n>" for the n threads that used the most CPU in the last second.
//    Every BOTS_CPU_SAMPLE_WINDOW seconds (default 10) it appends one window to BOTS_CPU_SAMPLE_FILE (default
//    cpu_samples.txt): "# window <n> <seconds since start> <seconds>", one "# thread <id> <name> samples=<n>
//    cpu_ms=<n>" line per thread, then folded stacks "<thread id>;<root frame>;...;<leaf frame> <count>", frames as
//    <module>+0x<offset>. Resolve them offline with llvm-symbolizer and the build's PDBs.
//    Nothing in the sampler allocates while a thread is suspended (the suspended thread could hold the heap lock).
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "thread_stats.h"

namespace bots {
namespace {

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

std::string ThreadName(HANDLE h) {
  static const auto fn = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription"));
  std::string out;
  PWSTR w = nullptr;
  if (fn && SUCCEEDED(fn(h, &w)) && w) {
    for (const wchar_t* p = w; *p; ++p) out += (*p < 128 && *p > 32) ? char(*p) : '_';
    LocalFree(w);
  }
  return out;
}

double FileTimeSeconds(const FILETIME& f) {
  return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;
}

std::vector<DWORD> ProcessThreadIds() {
  std::vector<DWORD> ids;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return ids;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    if (te.th32OwnerProcessID == pid) ids.push_back(te.th32ThreadID);
  CloseHandle(snap);
  return ids;
}

}  // namespace

std::map<uint64_t, ThreadCpu> SampleThreadCpuWin() {
  std::map<uint64_t, ThreadCpu> out;
  for (DWORD id : ProcessThreadIds()) {
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!h) continue;
    FILETIME c, e, k, u;
    if (GetThreadTimes(h, &c, &e, &k, &u)) out[id] = {ThreadName(h), FileTimeSeconds(k) + FileTimeSeconds(u)};
    CloseHandle(h);
  }
  return out;
}

namespace {

struct Module {
  uint64_t base, size;
  std::string name;
};

std::vector<Module> ListModules() {
  std::vector<Module> mods;
  HMODULE handles[1024];
  DWORD needed = 0;
  HANDLE proc = GetCurrentProcess();
  if (!EnumProcessModules(proc, handles, sizeof(handles), &needed)) return mods;
  for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
    MODULEINFO mi{};
    char name[MAX_PATH] = "?";
    GetModuleInfo(proc, handles[i], &mi, sizeof(mi));
    GetModuleBaseNameA(proc, handles[i], name, sizeof(name));
    mods.push_back({uint64_t(mi.lpBaseOfDll), mi.SizeOfImage, name});
  }
  std::sort(mods.begin(), mods.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
  return mods;
}

struct Target {
  DWORD id = 0;
  HANDLE handle = nullptr;
  std::string name;
  uint64_t stack_low = 0, stack_high = 0;
  double cpu_start = 0;  // CPU seconds at the window's start
  uint64_t samples = 0;
};

constexpr int kMaxFrames = 64;

struct StackKey {
  uint32_t thread;
  uint32_t depth;
  uint64_t frames[kMaxFrames];
  bool operator==(const StackKey& o) const {
    return thread == o.thread && depth == o.depth && !std::memcmp(frames, o.frames, depth * sizeof(uint64_t));
  }
};
struct StackHash {
  size_t operator()(const StackKey& k) const {
    uint64_t h = 1469598103934665603ull ^ k.thread;
    for (uint32_t i = 0; i < k.depth; ++i) h = (h ^ k.frames[i]) * 1099511628211ull;
    return size_t(h);
  }
};

// Stack bounds from the thread's TEB (NT_TIB StackBase / StackLimit), so the leaf-frame fallback only reads the stack.
bool StackBounds(HANDLE h, uint64_t& low, uint64_t& high) {
  struct THREAD_BASIC_INFORMATION_ {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    struct { HANDLE p, t; } ClientId;
    ULONG_PTR AffinityMask;
    LONG Priority, BasePriority;
  } tbi{};
  using Fn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static const auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
  if (!fn || fn(h, 0, &tbi, sizeof(tbi), nullptr) < 0 || !tbi.TebBaseAddress) return false;
  const NT_TIB* tib = static_cast<const NT_TIB*>(tbi.TebBaseAddress);
  high = uint64_t(tib->StackBase);
  low = uint64_t(tib->StackLimit) & ~uint64_t(0xFFFFF);  // the guard region moves down as the stack grows
  if (high > 0x100000 && high - low > (uint64_t(64) << 20)) low = high - (uint64_t(64) << 20);
  return high > low;
}

// Suspend, read the context, unwind, resume. No allocation in between.
uint32_t CaptureStack(Target& t, uint64_t* frames) {
  if (SuspendThread(t.handle) == DWORD(-1)) return 0;
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  uint32_t n = 0;
  if (GetThreadContext(t.handle, &ctx)) {
    frames[n++] = ctx.Rip;
    while (n < kMaxFrames) {
      DWORD64 image = 0;
      PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
      if (fe) {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fe, &ctx, &handler_data, &establisher, nullptr);
      } else {  // leaf function: the return address is at the stack pointer
        if (ctx.Rsp < t.stack_low || ctx.Rsp + 8 > t.stack_high) break;
        ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      }
      if (ctx.Rip < 0x10000 || ctx.Rsp < t.stack_low || ctx.Rsp > t.stack_high) break;
      frames[n++] = ctx.Rip;
    }
  }
  ResumeThread(t.handle);
  return n;
}

std::string Frame(const std::vector<Module>& mods, uint64_t pc) {
  auto it = std::upper_bound(mods.begin(), mods.end(), pc, [](uint64_t v, const Module& m) { return v < m.base; });
  char buf[160];
  if (it != mods.begin() && pc - (it - 1)->base < (it - 1)->size)
    std::snprintf(buf, sizeof(buf), "%s+0x%llx", (it - 1)->name.c_str(), (unsigned long long)(pc - (it - 1)->base));
  else
    std::snprintf(buf, sizeof(buf), "?+0x%llx", (unsigned long long)pc);
  return buf;
}

double ThreadCpuSeconds(HANDLE h) {
  FILETIME c, e, k, u;
  return GetThreadTimes(h, &c, &e, &k, &u) ? FileTimeSeconds(k) + FileTimeSeconds(u) : 0.0;
}

void SamplerMain(std::string spec, int hz, double window_s, std::string path) {
  SetThreadDescription(GetCurrentThread(), L"bots cpu sampler");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  // A high-resolution waitable timer: plain sleeps tick at the default 15.6 ms timer resolution.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  std::vector<std::string> wanted;
  int hot = 0;
  if (spec.rfind("hot:", 0) == 0) {
    hot = std::max(1, std::atoi(spec.c_str() + 4));
  } else {
    for (size_t s = 0; s <= spec.size();) {
      size_t e = spec.find(',', s);
      if (e == std::string::npos) e = spec.size();
      if (e > s) wanted.push_back(spec.substr(s, e - s));
      s = e + 1;
    }
  }
  const DWORD self = GetCurrentThreadId();
  std::map<DWORD, Target> targets;
  std::map<DWORD, double> last_cpu;  // for hot:<n>
  std::unordered_map<StackKey, uint32_t, StackHash> stacks;
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  auto window_start = t0, last_select = t0 - std::chrono::seconds(2);
  int window = 0;
  const auto period = std::chrono::microseconds(1000000 / std::max(1, hz));
  auto next = t0;
  StackKey key{};

  auto select = [&] {
    std::map<DWORD, double> cpu_now;
    std::vector<std::pair<double, DWORD>> by_cpu;
    for (DWORD id : ProcessThreadIds()) {
      if (id == self) continue;
      if (targets.count(id)) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
      if (!h) continue;
      const std::string name = ThreadName(h);
      bool take = false;
      if (hot) {
        const double c = ThreadCpuSeconds(h);
        cpu_now[id] = c;
        auto it = last_cpu.find(id);
        if (it != last_cpu.end()) by_cpu.push_back({c - it->second, id});
      } else {
        for (const auto& w : wanted) take = take || name.find(w) != std::string::npos;
      }
      if (take) {
        Target t;
        t.id = id, t.handle = h, t.name = name.empty() ? std::to_string(id) : name;
        t.cpu_start = ThreadCpuSeconds(h);
        if (StackBounds(h, t.stack_low, t.stack_high)) {
          targets[id] = t;
          continue;
        }
      }
      CloseHandle(h);
    }
    if (hot) {
      std::sort(by_cpu.rbegin(), by_cpu.rend());
      for (int i = 0; i < hot && i < int(by_cpu.size()); ++i) {
        const DWORD id = by_cpu[i].second;
        if (targets.size() >= size_t(hot)) break;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!h) continue;
        Target t;
        t.id = id, t.handle = h, t.name = ThreadName(h);
        if (t.name.empty()) t.name = std::to_string(id);
        t.cpu_start = ThreadCpuSeconds(h);
        if (StackBounds(h, t.stack_low, t.stack_high)) targets[id] = t;
        else CloseHandle(h);
      }
      last_cpu = std::move(cpu_now);
    }
  };

  auto flush = [&](clk::time_point now) {
    const auto mods = ListModules();
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    std::fprintf(f, "# window %d %.1f %.1f\n", window++, std::chrono::duration<double>(window_start - t0).count(),
                 std::chrono::duration<double>(now - window_start).count());
    for (auto& [id, t] : targets) {
      const double cpu = ThreadCpuSeconds(t.handle);
      std::fprintf(f, "# thread %lu %s samples=%llu cpu_ms=%.0f\n", (unsigned long)id, t.name.c_str(),
                   (unsigned long long)t.samples, (cpu - t.cpu_start) * 1000);
      t.cpu_start = cpu, t.samples = 0;
    }
    std::string line;
    for (const auto& [k, count] : stacks) {
      line = std::to_string(k.thread);
      for (int i = int(k.depth) - 1; i >= 0; --i) line += ';' + Frame(mods, k.frames[i]);
      std::fprintf(f, "%s %u\n", line.c_str(), count);
    }
    std::fclose(f);
    stacks.clear();
  };

  for (;;) {
    auto now = clk::now();
    if (now - last_select >= std::chrono::seconds(1)) {
      if (hot) {  // re-pick the hottest threads every window
        if (now - window_start >= std::chrono::duration<double>(window_s) || targets.empty()) {
          if (!targets.empty()) flush(now), window_start = now;
          for (auto& [id, t] : targets) CloseHandle(t.handle);
          targets.clear();
          select();
        } else if (last_cpu.empty()) {
          select();
        }
      } else {
        select();
      }
      last_select = now;
    }
    for (auto& [id, t] : targets) {
      key.thread = id;
      key.depth = CaptureStack(t, key.frames);
      if (!key.depth) continue;
      ++t.samples;
      ++stacks[key];
    }
    if (!hot && now - window_start >= std::chrono::duration<double>(window_s)) {
      flush(now);
      window_start = now;
    }
    next += period;
    now = clk::now();
    if (next > now) {
      LARGE_INTEGER due;
      due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count() / 100;
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
      else std::this_thread::sleep_until(next);
    }
    else next = now;
  }
}

}  // namespace

void StartCpuSampler() {
  const char* spec = std::getenv("BOTS_CPU_SAMPLE");
  if (!spec || !*spec || !std::strcmp(spec, "0")) return;
  const char* hz = std::getenv("BOTS_CPU_SAMPLE_HZ");
  const char* win = std::getenv("BOTS_CPU_SAMPLE_WINDOW");
  const char* file = std::getenv("BOTS_CPU_SAMPLE_FILE");
  std::thread(SamplerMain, std::string(spec), hz && *hz ? std::atoi(hz) : 500, win && *win ? std::atof(win) : 10.0,
              std::string(file && *file ? file : "cpu_samples.txt"))
      .detach();
}

}  // namespace bots

#endif  // _WIN32
