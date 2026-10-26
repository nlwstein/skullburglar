// Per-thread CPU time sampler for performance work: BOTS_THREAD_STATS=1 logs, every 5 s, each thread that used
// more than 2% of a core in that window ("thread cpu: <name> <percent>%"), plus the process total. Guest threads
// are named XThread<id>; the ids are stable from run to run, so two builds compare thread for thread.

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

#include <rex/logging.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#elif defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#endif

namespace bots {

struct ThreadCpu {
  std::string name;
  double seconds;
};

#if defined(_WIN32)
std::map<uint64_t, ThreadCpu> SampleThreadCpuWin();  // cpu_sampler_win.cpp
void StartCpuSampler();                               // BOTS_CPU_SAMPLE, cpu_sampler_win.cpp
#else
inline void StartCpuSampler() {}
#endif

// Cumulative CPU seconds per thread id.
inline std::map<uint64_t, ThreadCpu> SampleThreadCpu() {
  std::map<uint64_t, ThreadCpu> out;
#if defined(__APPLE__)
  thread_act_array_t threads;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return out;
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) == KERN_SUCCESS) {
      thread_identifier_info_data_t id;
      mach_msg_type_number_t m = THREAD_IDENTIFIER_INFO_COUNT;
      uint64_t tid = threads[i];
      if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&id), &m) == KERN_SUCCESS)
        tid = id.thread_id;
      char name[64] = "";
      if (pthread_t p = pthread_from_mach_thread_np(threads[i])) pthread_getname_np(p, name, sizeof name);
      double s = info.user_time.seconds + info.user_time.microseconds * 1e-6 + info.system_time.seconds +
                 info.system_time.microseconds * 1e-6;
      out[tid] = {name, s};
    }
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_t));
#elif defined(__linux__)
  if (DIR* d = opendir("/proc/self/task")) {
    const double hz = static_cast<double>(sysconf(_SC_CLK_TCK));
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::ifstream f(std::string("/proc/self/task/") + e->d_name + "/stat");
      std::string line;
      if (!std::getline(f, line)) continue;
      auto lp = line.find('('), rp = line.rfind(')');
      if (lp == std::string::npos || rp == std::string::npos) continue;
      std::istringstream rest(line.substr(rp + 2));
      std::string tok;
      unsigned long long ut = 0, st = 0;
      for (int i = 3; i <= 15 && (rest >> tok); ++i) {
        if (i == 14) ut = std::strtoull(tok.c_str(), nullptr, 10);
        if (i == 15) st = std::strtoull(tok.c_str(), nullptr, 10);
      }
      out[std::strtoull(e->d_name, nullptr, 10)] = {line.substr(lp + 1, rp - lp - 1), (ut + st) / hz};
    }
    closedir(d);
  }
#elif defined(_WIN32)
  out = SampleThreadCpuWin();
#endif
  return out;
}

inline void StartThreadStats() {
  StartCpuSampler();
  const char* env = std::getenv("BOTS_THREAD_STATS");
  if (!env || !*env || *env == '0') return;
  std::thread([] {
    using clk = std::chrono::steady_clock;
    auto prev = SampleThreadCpu();
    auto t0 = clk::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      auto cur = SampleThreadCpu();
      auto t1 = clk::now();
      double dt = std::chrono::duration<double>(t1 - t0).count();
      t0 = t1;
      std::string line;
      double total = 0;
      for (auto& [tid, c] : cur) {
        auto it = prev.find(tid);
        double used = c.seconds - (it == prev.end() ? 0.0 : it->second.seconds);
        total += used;
        if (used / dt > 0.02) {
          std::string nm = c.name.empty() ? std::to_string(tid) : c.name;
          for (char& ch : nm) if (ch == ' ') ch = '_';
          char buf[96];
          std::snprintf(buf, sizeof buf, " %s=%.0f%%", nm.c_str(), 100.0 * used / dt);
          line += buf;
        }
      }
      REXLOG_INFO("thread cpu ({:.1f} s): total={:.0f}%{}", dt, 100.0 * total / dt, line);
      prev = std::move(cur);
    }
  }).detach();
}

}  // namespace bots
