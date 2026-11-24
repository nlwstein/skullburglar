// Startup summary and the diagnostics bundle: see diagnostics.h.
#include "diagnostics.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>
#else
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <crt_externs.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#elif !defined(_WIN32)
extern char** environ;
#endif

#include "path_utf8.h"

namespace bots::diag {
namespace {

namespace fs = std::filesystem;

struct State {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::string> lines;  // "summary: key: value"
  int pending = 0;                 // background probes still running
};
State& St() {
  static State* s = new State;  // leaked on purpose: a probe thread may finish during exit
  return *s;
}

void Add(const std::function<void(const std::string&)>& log, std::string key, const std::string& value) {
  const std::string line = SummaryLine(key, value);
  {
    std::lock_guard lock(St().mu);
    St().lines.push_back(line);
  }
  if (log) log(line);
}

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

std::string Home() {
#if defined(_WIN32)
  return Env("USERPROFILE");
#else
  return Env("HOME");
#endif
}

std::vector<std::string> Environment() {
  std::vector<std::string> out;
#if defined(_WIN32)
  if (const wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
      const std::wstring w(p);
      const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
      std::string s(size_t(n > 0 ? n : 0), '\0');
      if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
      out.push_back(std::move(s));
    }
    FreeEnvironmentStringsW(const_cast<wchar_t*>(block));
  }
#elif defined(__APPLE__)
  for (char** e = *_NSGetEnviron(); e && *e; ++e) out.emplace_back(*e);
#else
  for (char** e = environ; e && *e; ++e) out.emplace_back(*e);
#endif
  return out;
}

std::string ReadFile(const fs::path& p, size_t max_bytes = 1 << 20) {
  std::ifstream f(p, std::ios::binary);
  std::string s(max_bytes, '\0');
  f.read(s.data(), std::streamsize(max_bytes));
  s.resize(size_t(f.gcount()));
  return s;
}

// A link (symlink, or a Windows junction / other reparse point) at `p` itself.
bool IsLink(const fs::path& p) {
  std::error_code ec;
  if (fs::is_symlink(fs::symlink_status(p, ec))) return true;
#if defined(_WIN32)
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return false;
#endif
}

std::string FsType(const fs::path& real) {
#if defined(_WIN32)
  wchar_t root[MAX_PATH + 1] = {};
  if (!GetVolumePathNameW(real.c_str(), root, MAX_PATH)) return "";
  wchar_t name[64] = {};
  if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, name, 63)) return "";
  std::string s;
  for (wchar_t* c = name; *c; ++c) s += char(*c < 128 ? ::tolower(int(*c)) : '?');
  const UINT type = GetDriveTypeW(root);
  const char* kind = type == DRIVE_REMOVABLE ? ", removable" : type == DRIVE_REMOTE ? ", network" : "";
  return s + kind;
#elif defined(__APPLE__)
  struct statfs sf{};
  if (statfs(real.c_str(), &sf) != 0) return "";
  std::string s = sf.f_fstypename;
  if (sf.f_flags & MNT_LOCAL) {
  } else {
    s += ", network";
  }
  return s;
#else
  return FsTypeNote(FsTypeFromMountInfo(ReadFile("/proc/self/mountinfo", 4 << 20), real.string()));
#endif
}

// ---- Platform facts ----

std::string OsName() {
#if defined(__APPLE__)
  auto sysctl_str = [](const char* name) {
    char buf[128] = {};
    size_t n = sizeof(buf) - 1;
    return sysctlbyname(name, buf, &n, nullptr, 0) == 0 ? std::string(buf) : std::string();
  };
  int translated = 0;
  size_t tn = sizeof(translated);
  const bool rosetta = sysctlbyname("sysctl.proc_translated", &translated, &tn, nullptr, 0) == 0 && translated == 1;
  return "macOS " + sysctl_str("kern.osproductversion") + " (build " + sysctl_str("kern.osversion") + ", " +
#if defined(__aarch64__)
         "arm64"
#else
         "x86_64"
#endif
         + (rosetta ? ", under Rosetta" : "") + ")";
#elif defined(_WIN32)
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof(v);
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) fn(&v);
  std::string display;
  {
    char buf[64] = {};
    DWORD n = sizeof(buf) - 1;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion",
                     RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS)
      display = buf;
  }
  std::string s = std::string("Windows ") + (v.dwBuildNumber >= 22000 ? "11" : "10") + " " + display + " (build " +
                  std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
                  std::to_string(v.dwBuildNumber) + ")";
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    if (GetProcAddress(nt, "wine_get_version")) s += ", running under Wine or Proton";
  return s;
#else
  std::string s = LinuxOsName(ParseKeyValues(ReadFile("/etc/os-release", 16384)));
  utsname u{};
  if (uname(&u) == 0) s += std::string(", kernel ") + u.release + " " + u.machine;
  if (const std::string t = Env("XDG_SESSION_TYPE"); !t.empty()) s += ", " + t + " session";
  return s;
#endif
}

std::string CpuModel(unsigned& cores, unsigned& threads) {
  threads = std::max(1u, std::thread::hardware_concurrency());
  cores = threads;
#if defined(__APPLE__)
  char buf[128] = {};
  size_t n = sizeof(buf) - 1;
  std::string model = sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0) == 0 ? buf : "unknown";
  int phys = 0;
  size_t pn = sizeof(phys);
  if (sysctlbyname("hw.physicalcpu", &phys, &pn, nullptr, 0) == 0 && phys > 0) cores = unsigned(phys);
  return model;
#elif defined(_WIN32)
  char buf[128] = {};
  DWORD n = sizeof(buf) - 1;
  std::string model = RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                   "ProcessorNameString", RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS
                          ? buf
                          : "unknown";
  DWORD len = 0;
  GetLogicalProcessorInformation(nullptr, &len);
  if (len) {
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (GetLogicalProcessorInformation(info.data(), &len)) {
      unsigned c = 0;
      for (const auto& i : info) c += i.Relationship == RelationProcessorCore;
      if (c) cores = c;
    }
  }
  // Trim the registry string's padding spaces.
  while (!model.empty() && model.back() == ' ') model.pop_back();
  model.erase(0, model.find_first_not_of(' '));
  return model;
#else
  const std::string cpuinfo = ReadFile("/proc/cpuinfo", 1 << 20);
  std::string model;
  std::vector<std::string> core_ids;
  std::string physical;
  std::istringstream in(cpuinfo);
  for (std::string line; std::getline(in, line);) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = line.substr(0, line.find_last_not_of(" \t", colon - 1) + 1);
    std::string value = line.substr(colon + 1);
    value.erase(0, value.find_first_not_of(' '));
    if (key == "model name" && model.empty()) model = value;
    else if (key == "Hardware" && model.empty()) model = value;
    else if (key == "physical id") physical = value;
    else if (key == "core id") {
      const std::string id = physical + "/" + value;
      if (std::find(core_ids.begin(), core_ids.end(), id) == core_ids.end()) core_ids.push_back(id);
    }
  }
  if (!core_ids.empty()) cores = unsigned(core_ids.size());
  return model.empty() ? "unknown" : model;
#endif
}

// Total and available memory in bytes (0 when unknown).
void Memory(uint64_t& total, uint64_t& available) {
  total = available = 0;
#if defined(__APPLE__)
  uint64_t mem = 0;
  size_t n = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &n, nullptr, 0) == 0) total = mem;
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  vm_size_t page = 0;
  host_page_size(mach_host_self(), &page);
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS)
    available = uint64_t(vm.free_count + vm.inactive_count + vm.speculative_count) * page;
#elif defined(_WIN32)
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  if (GlobalMemoryStatusEx(&m)) total = m.ullTotalPhys, available = m.ullAvailPhys;
#else
  std::istringstream in(ReadFile("/proc/meminfo", 8192));
  for (std::string line; std::getline(in, line);) {
    const uint64_t kb = std::strtoull(line.c_str() + line.find(':') + 1, nullptr, 10) * 1024;
    if (line.rfind("MemTotal:", 0) == 0) total = kb;
    else if (line.rfind("MemAvailable:", 0) == 0) available = kb;
  }
#endif
}

#if defined(_WIN32)
// Each display attached to the desktop as DXGI sees it: size, and whether Windows has HDR on for it (the BT.2020 PQ color
// space) with its peak, full-frame and minimum luminance (what an OLED or HDR monitor reports). Loaded at run time so
// the game doesn't link dxgi.
std::string DisplayDetails() {
  using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  if (!dxgi) return "";
  std::string out;
  if (auto create = reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))) {
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
      IDXGIAdapter1* adapter = nullptr;
      for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
          DXGI_OUTPUT_DESC d{};
          IDXGIOutput6* o6 = nullptr;
          if (SUCCEEDED(output->GetDesc(&d)) && d.AttachedToDesktop && SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&o6)))) {
            DXGI_OUTPUT_DESC1 d1{};
            if (SUCCEEDED(o6->GetDesc1(&d1))) {
              char buf[256];
              const bool hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
              std::snprintf(buf, sizeof(buf), "%s%ldx%ld, %s, peak %.0f nits, full-frame %.0f nits, black %.4f nits, %u bit",
                            out.empty() ? "" : "; ", long(d1.DesktopCoordinates.right - d1.DesktopCoordinates.left),
                            long(d1.DesktopCoordinates.bottom - d1.DesktopCoordinates.top),
                            hdr ? "Windows HDR on" : "Windows HDR off", double(d1.MaxLuminance), double(d1.MaxFullFrameLuminance),
                            double(d1.MinLuminance), d1.BitsPerColor);
              out += buf;
            }
            o6->Release();
          }
          output->Release();
        }
        adapter->Release();
      }
      factory->Release();
    }
  }
  FreeLibrary(dxgi);
  return out;
}
#else
std::string DisplayDetails() { return ""; }
#endif

fs::path RealPath(const fs::path& p) {
  std::error_code ec;
  fs::path r = fs::canonical(p, ec);
  return ec ? p : r;
}

std::string DescribeGameFolder(const fs::path& dir) {
  std::error_code ec;
  const fs::path real = RealPath(dir);
  std::string s = bots::PlainPathUtf8(real);
  std::vector<std::string> links;
  if (IsLink(dir)) links.push_back("the folder itself");
  else if (real.lexically_normal() != fs::absolute(dir, ec).lexically_normal()) links.push_back("a parent folder");
  // The same places the game reads from: the folder, C1Game and CookedXenon.
  size_t inside = 0;
  for (const fs::path& sub : {fs::path{}, fs::path{"C1Game"}, fs::path{"C1Game"} / "CookedXenon"}) {
    std::error_code e;
    for (fs::directory_iterator it(dir / sub, e), end; !e && it != end; it.increment(e)) {
      if (IsLink(it->path())) ++inside;
    }
  }
  if (inside) links.push_back(std::to_string(inside) + " link(s) inside it");
  const std::string type = FsType(real);
  s += " (filesystem " + (type.empty() ? std::string("unknown") : type) + "; links: ";
  if (links.empty()) s += "none";
  for (size_t i = 0; i < links.size(); ++i) s += (i ? ", " : "") + links[i];
  return s + ")";
}

ScrubInfo ScrubInfoHere() {
  ScrubInfo info;
  info.home = Home();
#if defined(_WIN32)
  info.user = Env("USERNAME");
  info.computer = Env("COMPUTERNAME");
#else
  info.user = Env("USER");
  if (info.user.empty()) info.user = Env("LOGNAME");
  char host[256] = {};
  if (gethostname(host, sizeof(host) - 1) == 0) info.computer = host;
  info.computer = info.computer.substr(0, info.computer.find('.'));  // "pat-mac.local" -> "pat-mac"
#endif
  return info;
}

std::string Stamp() {
  const std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  return buf;
}

}  // namespace

void StartStartupSummary(const StartInfo& info, std::function<void(const std::string&)> log) {
  const std::string home = Home();
  if (log)
    for (const std::string& line : EnvSwitchLines(Environment(), home)) log(line);
  {
    std::lock_guard lock(St().mu);
    ++St().pending;
  }
  std::thread([info, log = std::move(log), home] {
    const auto start = std::chrono::steady_clock::now();
    Add(log, "build", info.version);
    Add(log, "os", OsName());
    unsigned cores = 0, threads = 0;
    const std::string cpu = CpuModel(cores, threads);
    Add(log, "cpu", cpu + ", " + std::to_string(cores) + " cores / " + std::to_string(threads) + " threads");
    uint64_t total = 0, avail = 0;
    Memory(total, avail);
    Add(log, "memory", total ? GiB(total) + " total, " + (avail ? GiB(avail) : std::string("?")) + " available" : "unknown");
    const auto getenv_fn = [](const char* n) { return Env(n); };
    Add(log, "launch", LaunchContext(getenv_fn));
    Add(log, "install", InstallKind(info.program.generic_string(), getenv_fn) + " (" +
                            ShortenPath(bots::PlainPathUtf8(RealPath(info.program)), home) + ")");
    {
      std::string data = info.data_root.empty() ? "platform default" : ShortenPath(bots::PlainPathUtf8(info.data_root), home);
      if (!info.data_source.empty()) data += " (" + info.data_source + ")";
      Add(log, "data folder", data + (info.data_source == "portable.txt" ? ", portable mode" : ", not portable"));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (log) log("summary: startup probes took " + std::to_string(int(ms)) + " ms (background thread)");
    std::lock_guard lock(St().mu);
    --St().pending;
    St().cv.notify_all();
  }).detach();
}

void AddGameSummary(const GameInfo& info, std::function<void(const std::string&)> log) {
  Add(log, "renderer", info.renderer);
  Add(log, "display", info.display);
  Add(log, "hdr", info.hdr);
  Add(log, "controllers", info.controllers);
  Add(log, "gpu", "see the \"Vulkan device\" line in this log");
  {
    std::lock_guard lock(St().mu);
    ++St().pending;
  }
  std::thread([info, log = std::move(log)] {
    if (const std::string displays = DisplayDetails(); !displays.empty()) Add(log, "displays", displays);
    Add(log, "game folder", info.game_dir.empty() ? "unknown" : DescribeGameFolder(info.game_dir));
    std::lock_guard lock(St().mu);
    --St().pending;
    St().cv.notify_all();
  }).detach();
}

std::vector<std::string> SummaryLines() {
  std::unique_lock lock(St().mu);
  St().cv.wait_for(lock, std::chrono::seconds(2), [] { return St().pending == 0; });
  return St().lines;
}

fs::path SaveBundle(const fs::path& log_folder, size_t tail_lines, std::string& error) {
  error.clear();
  std::error_code ec;
  fs::create_directories(log_folder, ec);
  // The two newest logs (bots_*.log, the session's own and the one before it), newest first.
  std::vector<std::pair<fs::file_time_type, fs::path>> logs;
  for (fs::directory_iterator it(log_folder, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->path().extension() == ".log") {
      std::error_code e;
      logs.emplace_back(fs::last_write_time(it->path(), e), it->path());
    }
  }
  std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (logs.size() > 2) logs.resize(2);

  std::string out = "Skullburglar diagnostics, saved " + Stamp() + "\nAttach this file to your bug report.\n\n";
  const std::vector<std::string> lines = SummaryLines();
  out += "== Summary\n";
  for (const std::string& l : lines) out += l + "\n";
  if (lines.empty()) out += "(no summary yet)\n";
  for (size_t li = 0; li < logs.size(); ++li) {
    const fs::path& path = logs[li].second;
    const std::string text = ReadFile(path, 16 << 20);
    out += "\n== " + path.filename().string() + (li == 0 ? " (newest)" : " (the one before)") + "\n";
    {  // what each run said about itself at the top: summary, env switches and the GPU
      std::vector<std::string> head = LinesContaining(
          text, {"summary: ", "env: ", "Vulkan device '", "VulkanPresenter: Created", "driverName:", "driverInfo:"}, 60);
      if (!head.empty()) {
        out += "-- summary, switches and GPU lines from this log\n";
        for (const std::string& l : head) out += l + "\n";
        out += "-- end\n";
      }
    }
    out += "-- last " + std::to_string(tail_lines) + " lines\n" + LastLines(text, tail_lines);
    if (!out.empty() && out.back() != '\n') out += '\n';
  }
  out = ScrubText(out, ScrubInfoHere());  // the file goes into public issues
  const fs::path file = log_folder / "skullburglar-diagnostics.txt";
  std::ofstream f(file, std::ios::binary | std::ios::trunc);
  if (!f || !(f << out)) {
    error = "Could not write " + bots::PlainPathUtf8(file);
    return {};
  }
  return file;
}

}  // namespace bots::diag
