#include "host_stats.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <powrprof.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <sys/resource.h>
#include <fstream>
#endif

namespace bots::gfx::host {
namespace {

// Substrings of module names (lower case) that belong to overlays, recorders and injectors.
constexpr const char* kOverlayNames[] = {"rtss",       "riva",     "graphics-hook", "obs-",    "discordhook",
                                         "gameoverlay", "reshade", "specialk",      "fraps",   "bandicam",
                                         "action_x64", "nvspcap",  "afterburner",   "mangohud", "vkbasalt",
                                         "overlay",    "hook"};

bool IsOverlay(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (const char* s : kOverlayNames) {
    if (name.find(s) != std::string::npos) return true;
  }
  return false;
}

#if !defined(_WIN32)
uint64_t ReadNumber(const std::string& path) {
  std::ifstream f(path);
  uint64_t v = 0;
  f >> v;
  return v;
}
std::string ReadLine(const std::string& path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}
#endif

}  // namespace

CpuTimes ProcessCpuTimes() {
  CpuTimes t;
#if defined(_WIN32)
  FILETIME create, exit, kernel, user;
  if (GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) {
    auto us = [](const FILETIME& f) { return ((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10; };
    t.user_us = us(user), t.kernel_us = us(kernel);
  }
#else
  rusage r{};
  if (getrusage(RUSAGE_SELF, &r) == 0) {
    t.user_us = uint64_t(r.ru_utime.tv_sec) * 1000000 + r.ru_utime.tv_usec;
    t.kernel_us = uint64_t(r.ru_stime.tv_sec) * 1000000 + r.ru_stime.tv_usec;
  }
#endif
  return t;
}

CpuClock CpuClockMhz() {
  CpuClock c;
#if defined(_WIN32)
  struct ProcessorPowerInformation {  // PROCESSOR_POWER_INFORMATION (not in the SDK headers)
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
  };
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  std::vector<ProcessorPowerInformation> info(si.dwNumberOfProcessors);
  if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, info.data(),
                             ULONG(info.size() * sizeof(ProcessorPowerInformation))) == 0 &&
      !info.empty()) {
    uint64_t cur = 0, limit = 0;
    for (const auto& p : info) cur += p.CurrentMhz, limit += p.MhzLimit, c.max_mhz = std::max<uint32_t>(c.max_mhz, p.MaxMhz);
    c.current_mhz = uint32_t(cur / info.size()), c.limit_mhz = uint32_t(limit / info.size());
  }
#elif defined(__linux__)
  uint64_t cur = 0, n = 0;
  for (int i = 0; i < 1024; ++i) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
    const uint64_t khz = ReadNumber(base + "scaling_cur_freq");
    if (!khz) break;
    cur += khz, ++n;
    c.max_mhz = std::max<uint32_t>(c.max_mhz, uint32_t(ReadNumber(base + "cpuinfo_max_freq") / 1000));
    c.limit_mhz = std::max<uint32_t>(c.limit_mhz, uint32_t(ReadNumber(base + "scaling_max_freq") / 1000));
  }
  if (n) c.current_mhz = uint32_t(cur / n / 1000);
#endif
  return c;
}

double CpuProbeMicros() {
  // A dependent chain of multiplies and shifts the compiler can't fold: ~1 ms on a Steam Deck (kept short: it runs on
  // the GPU thread, inside a frame).
  const auto start = std::chrono::steady_clock::now();
  volatile uint64_t sink = 0;
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < 500000; ++i) x = (x ^ (x >> 29)) * 0xBF58476D1CE4E5B9ull + i;
  sink = x;
  (void)sink;
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
}

#if defined(_WIN32)
static std::string GuidString(const GUID& g) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned long>(g.Data1),
                g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6],
                g.Data4[7]);
  return buf;
}
#endif

std::string PowerState() {
  std::string out;
#if defined(_WIN32)
  SYSTEM_POWER_STATUS ps{};
  if (GetSystemPowerStatus(&ps)) {
    if (ps.ACLineStatus == 1) {
      out = "AC";
    } else if (ps.ACLineStatus == 0) {
      out = "battery";
      if (ps.BatteryLifePercent <= 100) out += " " + std::to_string(ps.BatteryLifePercent) + "%";
    }
    if (ps.SystemStatusFlag & 1) out += out.empty() ? "battery saver" : ", battery saver";
  }
  GUID* scheme = nullptr;
  if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
    // A player's log (#12) showed no plan with a fixed buffer: ask for the size first, and show the GUID if the
    // name still can't be read.
    std::string plan;
    DWORD size = 0;
    if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size) {
      std::vector<wchar_t> name(size / sizeof(wchar_t) + 1, L'\0');
      if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR*>(name.data()), &size) ==
          ERROR_SUCCESS) {
        char utf8[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, name.data(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        plan = utf8;
      }
    }
    out += (out.empty() ? "plan " : ", plan ") + (plan.empty() ? GuidString(*scheme) : plan);
    LocalFree(scheme);
  }
  // Windows 10/11's power mode (Settings > System > Power & battery) is an overlay on the plan, read through an
  // export the SDK headers don't declare.
  using GetOverlay = DWORD(WINAPI*)(GUID*);
  static const auto get_overlay = [] {
    HMODULE m = GetModuleHandleA("powrprof.dll");
    return m ? reinterpret_cast<GetOverlay>(reinterpret_cast<void*>(GetProcAddress(m, "PowerGetEffectiveOverlayScheme")))
             : nullptr;
  }();
  GUID overlay{};
  if (get_overlay && get_overlay(&overlay) == ERROR_SUCCESS) {
    const std::string id = GuidString(overlay);
    const char* mode = id == "00000000-0000-0000-0000-000000000000"   ? "balanced"
                       : id == "961cc777-2547-4f9d-8174-7d86181b8a7a" ? "best power efficiency"
                       : id == "ded574b5-45a0-4f42-8737-46345c09c238" ? "best performance"
                                                                      : nullptr;
    out += (out.empty() ? "power mode " : ", power mode ") + (mode ? std::string(mode) : id);
  }
#elif defined(__linux__)
  if (DIR* d = opendir("/sys/class/power_supply")) {
    while (dirent* e = readdir(d)) {
      const std::string name = e->d_name, base = "/sys/class/power_supply/" + name + "/";
      if (name[0] == '.') continue;
      const std::string type = ReadLine(base + "type");
      if (type == "Mains" && ReadNumber(base + "online")) out = out.empty() ? "AC" : "AC, " + out;
      if (type == "Battery") {
        const std::string b = "battery " + std::to_string(ReadNumber(base + "capacity")) + "% " + ReadLine(base + "status");
        out += out.empty() ? b : ", " + b;
      }
    }
    closedir(d);
  }
  const std::string profile = ReadLine("/sys/firmware/acpi/platform_profile");
  if (!profile.empty()) out += (out.empty() ? "profile " : ", profile ") + profile;
#endif
  return out;
}

std::string OverlayModules() {
  std::set<std::string> found;
#if defined(_WIN32)
  std::vector<HMODULE> modules(1024);
  DWORD needed = 0;
  if (K32EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)), &needed)) {
    modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
    for (HMODULE m : modules) {
      char name[MAX_PATH] = {};
      if (K32GetModuleBaseNameA(GetCurrentProcess(), m, name, sizeof(name)) && IsOverlay(name)) found.insert(name);
    }
  }
#elif defined(__linux__)
  std::ifstream maps("/proc/self/maps");
  for (std::string line; std::getline(maps, line);) {
    const size_t slash = line.rfind('/');
    if (slash != std::string::npos && IsOverlay(line.substr(slash + 1))) found.insert(line.substr(slash + 1));
  }
#endif
  std::string out;
  for (const auto& name : found) out += (out.empty() ? "" : ", ") + name;
  return out;
}

}  // namespace bots::gfx::host
