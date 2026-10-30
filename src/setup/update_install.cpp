// See update_install.h.
#include "update_install.h"

#include "update_check.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>  // renamex_np
#if TARGET_OS_OSX
#include <CoreFoundation/CoreFoundation.h>
#endif
#endif
// tvOS has no posix_spawn (and nothing to update in place): Run and RelaunchAfterExit do nothing there.
#if defined(__APPLE__) && TARGET_OS_TV
#define BOTS_UPDATE_NO_SPAWN 1
#endif
extern char** environ;
#endif

namespace bots::setup {

namespace fs = std::filesystem;

std::string PathToUtf8(const fs::path& p) {
#if defined(_WIN32)
  // path::string() converts through the ANSI code page and throws for a name it can't hold (any Cyrillic or CJK name).
  const std::wstring w = p.wstring();
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
  return out;
#else
  return p.string();
#endif
}

namespace {

#if defined(_WIN32)
constexpr const char* kOs = "windows";
#elif defined(__APPLE__)
constexpr const char* kOs = "macos";
#else
constexpr const char* kOs = "linux";
#endif

constexpr const char* kStagePrefix = "skullburglar-update-";
constexpr const char* kSwapPrefix = ".skullburglar-new-";

std::atomic<bool> g_swap_in_progress{false};

// Raises the "don't close the window now" flag for as long as files are being replaced.
struct SwapGuard {
  SwapGuard() { g_swap_in_progress.store(true); }
  ~SwapGuard() { g_swap_in_progress.store(false); }
};

void SetStage(InstallProgress& p, UpdateStage s) { p.stage.store(int(s)); }

[[maybe_unused]] std::string Err(const std::error_code& ec) { return ec.message(); }

// The system tools by absolute path on macOS (a PATH entry ahead of /usr/bin must not stand in for them).
[[maybe_unused]] std::string Tool(const std::string& name) {
#if defined(__APPLE__)
  static const char* const kBin[] = {"curl", "hdiutil", "ditto", "codesign", "open", "sh", "sleep"};
  for (const char* b : kBin)
    if (name == b) return std::string(name == "sh" ? "/bin/" : name == "sleep" ? "/bin/" : "/usr/bin/") + name;
  if (name == "diskutil") return "/usr/sbin/diskutil";
#else
  if (name == "sh") return "/bin/sh";
#endif
  return name;
}

#if defined(_WIN32)
std::string Sfx() { return std::to_string(GetCurrentProcessId()); }
#endif

// A fresh, empty folder under `base` that only this user can enter (mkdtemp on Unix: a new 0700 directory, never
// one that existed or a symlink). Empty path with `error` set otherwise.
fs::path MakeUniqueDir(const fs::path& base, const std::string& prefix, std::string& error) {
#if defined(_WIN32)
  static std::atomic<unsigned> counter{0};
  for (int attempt = 0; attempt < 50; ++attempt) {
    char name[64];
    std::snprintf(name, sizeof(name), "%s%08lx%04x", prefix.c_str(),
                  static_cast<unsigned long>(GetTickCount64() ^ (GetCurrentProcessId() << 8)), ++counter & 0xffff);
    fs::path dir = base / name;
    if (CreateDirectoryW(dir.c_str(), nullptr)) return dir;  // fails when it exists: nothing is ever reused
    if (GetLastError() != ERROR_ALREADY_EXISTS) break;
  }
  error = "can't create a folder in " + PathToUtf8(base);
  return {};
#else
  std::string tmpl = (base / (prefix + "XXXXXX")).string();
  if (!mkdtemp(tmpl.data())) {
    error = "can't create a folder in " + PathToUtf8(base) + ": " + std::strerror(errno);
    return {};
  }
  struct stat st {};
  if (lstat(tmpl.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
    error = "the new folder " + tmpl + " isn't private";
    return {};
  }
  return tmpl;
#endif
}

// Whether files can be created in `dir` (the swap and the staging need it; a read-only install can't update itself).
bool Writable(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
#if defined(_WIN32)
  std::string error;
  fs::path probe = MakeUniqueDir(dir, ".update-probe-", error);
  if (probe.empty()) return false;
  fs::remove(probe, ec);
  return true;
#else
  std::string tmpl = (dir / ".update-probe-XXXXXX").string();
  int fd = mkstemp(tmpl.data());
  if (fd < 0) return false;
  close(fd);
  unlink(tmpl.c_str());
  return true;
#endif
}

#if defined(__APPLE__) && TARGET_OS_OSX
// An app that is still quarantined (run from Downloads, or its first launches after being copied out of a disk image)
// runs from a random read-only copy under .../AppTranslocation/. Security.framework's (undocumented, but long-standing
// and used by other updaters) SecTranslocateCreateOriginalPathForURL names where it really is, which is where the
// update goes. Empty when it can't be resolved.
fs::path OriginalOfTranslocated(const fs::path& translocated) {
  void* sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
  if (!sec) return {};
  using Fn = CFURLRef (*)(CFURLRef, CFErrorRef*);
  Fn original_for = reinterpret_cast<Fn>(dlsym(sec, "SecTranslocateCreateOriginalPathForURL"));
  if (!original_for) return {};
  const std::string in = translocated.string();
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(in.c_str()),
                                                         CFIndex(in.size()), true);
  if (!url) return {};
  CFErrorRef err = nullptr;
  CFURLRef orig = original_for(url, &err);
  CFRelease(url);
  if (err) CFRelease(err);
  if (!orig) return {};
  char buf[4096];
  bool ok = CFURLGetFileSystemRepresentation(orig, true, reinterpret_cast<UInt8*>(buf), sizeof(buf));
  CFRelease(orig);
  return ok ? fs::path(buf) : fs::path();
}
#endif

fs::path SelfExe() {
#if defined(_WIN32)
  wchar_t buf[32768];
  DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return n ? fs::path(std::wstring(buf, n)) : fs::path();
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t n = sizeof(buf);
  if (_NSGetExecutablePath(buf, &n) != 0) return {};
  std::error_code ec;
  fs::path p = fs::canonical(buf, ec);
  if (ec) p = buf;
#if TARGET_OS_OSX
  if (p.string().find("/AppTranslocation/") != std::string::npos) {
    // .../d/Skullburglar.app/Contents/MacOS/bots -> the bundle -> where it really is
    fs::path orig = OriginalOfTranslocated(p.parent_path().parent_path().parent_path());
    if (!orig.empty()) return orig / "Contents" / "MacOS" / p.filename();
  }
#endif
  return p;
#else
  std::error_code ec;
  fs::path p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::path() : p;
#endif
}

#if !defined(_WIN32)
// Runs a program (named as Tool() gives it) without a shell. Output goes to `out` (a new file; never an existing one)
// or nowhere. `tick` runs about every 100 ms while it works and returns true to stop the program; `cancel` stops it
// too. Returns its exit status, or -1 when it didn't start, was stopped or failed.
int Run(const std::vector<std::string>& args, const fs::path& out = {}, const std::atomic<bool>* cancel = nullptr,
        const std::function<bool()>& tick = {}) {
#ifdef BOTS_UPDATE_NO_SPAWN
  (void)args, (void)out, (void)cancel, (void)tick;
  return -1;
#else
  std::vector<char*> argv;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  if (out.empty()) {
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  } else {
    const std::string out_path = out.string();
    posix_spawn_file_actions_addopen(&fa, 1, out_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  }
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  pid_t pid;
  int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) return -1;
  for (;;) {
    int status;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (w < 0) return -1;
    if ((cancel && cancel->load()) || (tick && tick())) {
      kill(pid, SIGTERM);
      waitpid(pid, &status, 0);
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#endif
}

// Starts a program that outlives this process, a second from now (so this one has quit).
// `cwd` (when given) is where it starts: the old folder this process runs in is about to disappear. `remove_dir` (when
// given, and one of our own stage folders) is deleted a second after this process has gone, as the new one starts.
void RelaunchAfterExit(const std::vector<std::string>& cmd, const fs::path& cwd = {}, const fs::path& remove_dir = {}) {
#ifdef BOTS_UPDATE_NO_SPAWN
  (void)cmd, (void)cwd, (void)remove_dir;
#else
  std::vector<std::string> args = {Tool("sh"), "-c", Tool("sleep") + " 1; exec \"$@\"", "sh"};
  if (!cwd.empty() && !remove_dir.empty() && remove_dir.filename().string().starts_with(".skullburglar-update-")) {
    args[2] = Tool("sleep") + " 1; cd -- \"$1\" || cd /; rm -rf -- \"$2\" & shift 2; exec \"$@\"";
    args.push_back(cwd.string());
    args.push_back(remove_dir.string());
  }
  args.insert(args.end(), cmd.begin(), cmd.end());
  std::vector<char*> argv;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
  pid_t pid;
  posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
#endif
}

// curl ships with macOS and practically every Linux, SteamOS included. Not a browser: it sets no quarantine mark.
// https only (http for a local test server, when the build allows it), never redirected to another scheme, and never
// more than the listed size.
std::string Download(const Asset& asset, const fs::path& dest, InstallProgress& p) {
  p.total = asset.size;
  p.done = 0;
  const char* proto = asset.url.starts_with("https://") ? "=https" : "=http";
  std::vector<std::string> args = {Tool("curl"), "-fsSL", "--retry", "2", "--connect-timeout", "15", "--max-time", "1800",
                                   "--proto", proto, "--proto-redir", proto, "--max-filesize", std::to_string(asset.size),
                                   "-o", dest.string(), asset.url};
  bool too_big = false;
  int rc = Run(args, {}, &p.cancel, [&] {
    std::error_code ec;
    uint64_t n = fs::file_size(dest, ec);
    if (!ec) {
      p.done = n;
      if (n > asset.size) too_big = true;
    }
    return too_big;
  });
  if (p.cancel) return "cancelled";
  if (too_big) return "the download is larger than listed";
  if (rc != 0) return "the download failed (curl " + std::to_string(rc) + ")";
  p.done = asset.size;
  return {};
}
#else
std::wstring Widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// WinHTTP writes the file itself: no browser, no Mark of the Web (Zone.Identifier stream), no SmartScreen check.
std::string Download(const Asset& asset, const fs::path& dest, InstallProgress& p) {
  p.total = asset.size;
  p.done = 0;
  std::wstring wurl = Widen(asset.url);
  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256], path[4096];
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = DWORD(std::size(path));
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return "bad download address";
  std::string error;
  HINTERNET session = WinHttpOpen(L"Skullburglar", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr;
  if (request) WinHttpSetTimeouts(request, 15000, 15000, 30000, 30000);
  DWORD status = 0, size = sizeof(status);
  if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr) ||
      !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                           &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    error = "the download failed (error " + std::to_string(GetLastError()) + ")";
  } else if (status != 200) {
    error = "the download failed (HTTP " + std::to_string(status) + ")";
  } else {
    std::ofstream out(dest, std::ios::binary);
    if (!out) error = "can't write " + PathToUtf8(dest);
    std::vector<char> buf(1 << 16);
    DWORD got = 0;
    while (error.empty()) {
      if (p.cancel) {
        error = "cancelled";
        break;
      }
      if (!WinHttpReadData(request, buf.data(), DWORD(buf.size()), &got)) {
        error = "the download failed (error " + std::to_string(GetLastError()) + ")";
        break;
      }
      if (!got) break;
      out.write(buf.data(), got);
      p.done += got;
      if (p.done > asset.size) {
        error = "the download is larger than listed";
        break;
      }
    }
    out.close();
    if (error.empty() && !out) error = "can't write " + PathToUtf8(dest);
  }
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return error;
}
#endif

// ---- Per-install steps ----

#if defined(_WIN32)
fs::path AppDataFolder() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, DWORD(std::size(buf)));
  if (!n || n >= std::size(buf)) return {};
  return fs::path(buf) / "bots-recomp";
}

// Written before the setup program runs, deleted by it when it finishes: still there at the next start means the
// update didn't complete (see TakeFailedUpdateNote).
fs::path PendingMarker() {
  fs::path d = AppDataFolder();
  return d.empty() ? d : d / "update-pending.txt";
}

// Whether `dir` was made by the setup program: it has uninstall.exe, and the setup's registry entry names this
// folder. An unzipped copy, or a build run from a source tree, has neither, and the setup must never run over it
// (it deletes program files first).
bool WindowsIsSetupInstall(const fs::path& dir) {
  std::error_code ec;
  if (dir.empty() || !fs::is_regular_file(dir / "uninstall.exe", ec)) return false;
  wchar_t buf[2048];
  DWORD bytes = sizeof(buf) - sizeof(wchar_t);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Skullburglar", L"InstallDir", RRF_RT_REG_SZ, nullptr, buf, &bytes) !=
      ERROR_SUCCESS)
    return false;
  return fs::equivalent(fs::path(buf), dir, ec) && !ec;
}

// The per-user setup program (packaging/windows/installer.nsi): /S silent, /UPDATE start the game afterwards (its
// finish page doesn't run when silent), /D= the install folder, which has to come last and unquoted. The setup
// program waits for this process to exit before it replaces bots.exe, and keeps saves, settings and game files.
bool InstallWindows(const fs::path& dir, const fs::path& setup, const Asset& asset, std::string& error) {
  // The setup deletes the old program files before it writes the new ones: be sure there is room first.
  ULARGE_INTEGER avail{};
  const uint64_t need = asset.size * 3 + (200ull << 20);
  if (GetDiskFreeSpaceExW(dir.c_str(), &avail, nullptr, nullptr) && avail.QuadPart < need) {
    error = "not enough free disk space (" + std::to_string(need >> 20) + " MB needed)";
    return false;
  }
  const fs::path marker = PendingMarker();
  if (!marker.empty()) {
    std::error_code ec;
    fs::create_directories(marker.parent_path(), ec);
    std::ofstream(marker, std::ios::trunc) << asset.version << "\n";
  }
  std::wstring cmd = L"\"" + setup.wstring() + L"\" /S /UPDATE /D=" + dir.wstring();
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                      nullptr, setup.parent_path().c_str(), &si, &pi)) {
    error = "can't start the setup program (error " + std::to_string(GetLastError()) + ")";
    std::error_code ec;
    if (!marker.empty()) fs::remove(marker, ec);
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}
#else
// A file's bytes into a new file `to` (never an existing one or a symlink), mode 0755, flushed.
bool CopyNewExecutable(const fs::path& from, const fs::path& to, std::string& error) {
  int in = open(from.c_str(), O_RDONLY | O_NOFOLLOW);
  if (in < 0) {
    error = "can't read the download";
    return false;
  }
  int out = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0700);
  if (out < 0) {
    close(in);
    error = "can't create " + to.string() + ": " + std::strerror(errno);
    return false;
  }
  std::vector<char> buf(1 << 20);
  bool ok = true;
  for (;;) {
    ssize_t n = read(in, buf.data(), buf.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = n == 0;
      break;
    }
    for (ssize_t off = 0; off < n;) {
      ssize_t w = write(out, buf.data() + off, size_t(n - off));
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        ok = false;
        break;
      }
      off += w;
    }
    if (!ok) break;
  }
  ok = ok && fchmod(out, 0755) == 0 && fsync(out) == 0;
  close(in);
  if (close(out) != 0) ok = false;
  if (!ok) {
    error = "can't copy the update next to the AppImage";
    std::error_code ec;
    fs::remove(to, ec);  // no half-written copy is left behind
  }
  return ok;
}

// macOS: the new app is copied out of the disk image beside the running one (same volume), checked with codesign, and
// swapped in. A rename-swap does it in one atomic step where the file system has it (APFS); otherwise the old app
// steps aside first and goes back if the new one can't be put in place.
bool InstallMac(const fs::path& bundle, const fs::path& dmg, const fs::path& tmp, std::string& error) {
  std::error_code ec;
  const fs::path parent = bundle.parent_path();
  const fs::path mnt = tmp / "mnt";
  fs::create_directories(mnt, ec);
  // hdiutil is deprecated on the newest macOS (diskutil image replaces it); either mounts it read-only, out of sight.
  bool via_hdiutil = Run({Tool("hdiutil"), "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mnt.string(),
                          dmg.string()}) == 0;
  if (!via_hdiutil &&
      Run({Tool("diskutil"), "image", "attach", "--nobrowse", "--readOnly", "--mountPoint", mnt.string(), dmg.string()}) != 0) {
    error = "can't open the disk image";
    return false;
  }
  std::string stage_error;
  const fs::path stage = MakeUniqueDir(parent, kSwapPrefix, stage_error);
  const fs::path fresh = stage / bundle.filename();
  bool copied = false;
  if (!stage.empty()) {
    for (const auto& e : fs::directory_iterator(mnt, ec)) {
      if (e.path().extension() == ".app" && fs::is_directory(e.path(), ec)) {
        copied = Run({Tool("ditto"), e.path().string(), fresh.string()}) == 0;
        break;
      }
    }
  }
  if (via_hdiutil) {
    if (Run({Tool("hdiutil"), "detach", "-quiet", mnt.string()}) != 0) Run({Tool("hdiutil"), "detach", "-force", "-quiet", mnt.string()});
  } else {
    Run({Tool("diskutil"), "eject", mnt.string()});
  }
  auto drop = [&] {
    std::error_code e;
    if (!stage.empty()) fs::remove_all(stage, e);
  };
  if (stage.empty()) {
    error = stage_error;
    return false;
  }
  if (!copied) {
    error = "no app in the disk image";
    drop();
    return false;
  }
  if (Run({Tool("codesign"), "--verify", "--deep", "--strict", fresh.string()}) != 0) {
    error = "the new app's signature is broken";
    drop();
    return false;
  }
  // macOS's App Management protection can refuse to change an app bundle; both ways below then fail cleanly, with the
  // old app untouched.
#if defined(__APPLE__)
  if (renamex_np(fresh.c_str(), bundle.c_str(), RENAME_SWAP) == 0) {
    drop();  // the old app is in the stage folder now
    return true;
  }
#endif
  const fs::path old = stage / "previous.app";
  fs::rename(bundle, old, ec);
  if (ec) {
    error = "can't move the old app aside: " + Err(ec);
    drop();
    return false;
  }
  fs::rename(fresh, bundle, ec);
  if (ec) {
    std::error_code ec2;
    fs::rename(old, bundle, ec2);  // roll back
    error = "can't put the new app in place: " + Err(ec) + (ec2 ? "; and can't restore the old one" : " (old app restored)");
    if (!ec2) drop();  // the old app is back; if not, keep it where it is
    return false;
  }
  drop();
  return true;
}

// Linux AppImage: the file is replaced by renaming the new one over it, which is atomic: either the old or the new
// file is there, and the running copy keeps its own inode.
bool InstallAppImage(const fs::path& image, const fs::path& fresh_download, std::string& error) {
  std::error_code ec;
  std::string tmpl = (image.parent_path() / (std::string(kSwapPrefix) + "XXXXXX")).string();
  int fd = mkstemp(tmpl.data());  // a new file, never an existing path or a symlink
  if (fd < 0) {
    error = "can't create a file next to the AppImage: " + std::string(std::strerror(errno));
    return false;
  }
  close(fd);
  unlink(tmpl.c_str());
  const fs::path staged = tmpl;
  if (!CopyNewExecutable(fresh_download, staged, error)) return false;
  fs::rename(staged, image, ec);
  if (ec) {
    error = "can't replace the AppImage: " + Err(ec);
    fs::remove(staged, ec);
    return false;
  }
  return true;
}

#if defined(__linux__)
// ---- Linux tarball ----
// The unpacked folder is replaced as a whole. The new folder is built next to it (same file system), checked, given
// whatever the player keeps in the old one, and then swapped in with one renameat2(RENAME_EXCHANGE): the install path
// is always the entire old folder or the entire new one. Where the file system can't exchange, nothing is attempted
// (two renames would leave a moment with no folder at the path, which a killed process or a Steam shortcut would
// hit), and the launcher keeps "Download it".

constexpr const char* kTarballMarker = "install-kind";  // shipped in the tarball only (tools/ci/package.sh)
constexpr const char* kTarballMarkerText = "linux-tarball";
constexpr const char* kTarballStagePrefix = ".skullburglar-update-";

// Test builds (tools/update_probe_test.sh defines BOTS_UPDATE_TESTING; a release never does): force a file system
// without exchange, and stop at a named point so the test can kill the process there.
[[maybe_unused]] bool TestSwitch(const char* name) {
#ifdef BOTS_UPDATE_TESTING
  const char* v = std::getenv(name);
  return v && *v;
#else
  (void)name;
  return false;
#endif
}

void TestHold(const char* point) {
#ifdef BOTS_UPDATE_TESTING
  const char* hold = std::getenv("BOTS_UPDATE_TEST_HOLD");
  if (!hold || !*hold || std::strncmp(hold, point, std::strlen(point)) != 0 || hold[std::strlen(point)] != ':') return;
  std::ofstream(hold + std::strlen(point) + 1) << point << "\n";  // "point:<file>": announce, then wait to be killed
  std::this_thread::sleep_for(std::chrono::seconds(60));
#else
  (void)point;
#endif
}

#ifndef SYS_renameat2
#if defined(__x86_64__)
#define SYS_renameat2 316
#elif defined(__aarch64__)
#define SYS_renameat2 276
#endif
#endif

// 0 when the two directories traded places, else the errno (ENOSYS or EINVAL: no exchange on this system or file system).
int ExchangeDirs(const fs::path& a, const fs::path& b) {
  if (TestSwitch("BOTS_UPDATE_TEST_NO_EXCHANGE")) return ENOSYS;
#ifdef SYS_renameat2
  if (syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), 2u /* RENAME_EXCHANGE */) == 0) return 0;
  return errno;
#else
  return ENOSYS;
#endif
}

// Whether this file system can exchange two folders in `parent` (tried on two empty scratch folders).
std::string CheckExchange(const fs::path& parent) {
  std::string error;
  fs::path a = MakeUniqueDir(parent, kTarballStagePrefix, error);
  if (a.empty()) return error;
  fs::path b = MakeUniqueDir(parent, kTarballStagePrefix, error);
  std::error_code ec;
  int rc = b.empty() ? 0 : ExchangeDirs(a, b);
  fs::remove(a, ec);
  if (!b.empty()) fs::remove(b, ec);
  if (b.empty()) return error;
  if (rc == ENOSYS || rc == EINVAL) return parent.string() + " is on a file system that can't swap folders in one step";
  return rc == 0 ? std::string() : "can't swap folders in " + parent.string() + ": " + std::strerror(rc);
}

// The tarball install marker next to the executable: a regular file (never a link) saying "linux-tarball".
bool HasTarballMarker(const fs::path& dir) {
  if (dir.empty()) return false;
  const std::string path = (dir / kTarballMarker).string();
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return false;
  char buf[32] = {};
  struct stat st {};
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && read(fd, buf, sizeof(buf) - 1) > 0 &&
            std::strncmp(buf, kTarballMarkerText, std::strlen(kTarballMarkerText)) == 0;
  close(fd);
  return ok;
}

uint64_t FreeBytes(const fs::path& dir) {
  struct statvfs sv {};
  if (statvfs(dir.c_str(), &sv) != 0) return UINT64_MAX;  // unknown: don't block
  return uint64_t(sv.f_bavail) * uint64_t(sv.f_frsize);
}

std::string ReadSmallFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return text.size() > (64u << 20) ? std::string() : text;
}

// Everything in the unpacked tree must be a plain file, folder or a link that stays inside the tree; the set-uid and
// sticky bits are dropped.
std::string CheckTreeIsPlain(const fs::path& root, const fs::path& dir) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    struct stat st {};
    if (lstat(e.path().c_str(), &st) != 0) return "can't read the unpacked update";
    if (S_ISLNK(st.st_mode)) {
      std::error_code lec;
      fs::path target = fs::read_symlink(e.path(), lec);
      fs::path inside = (e.path().parent_path() / target).lexically_normal().lexically_relative(root);
      if (lec || target.is_absolute() || inside.empty() || *inside.begin() == "..") return "the update has a link that leaves its folder";
    } else if (S_ISDIR(st.st_mode)) {
      if (std::string bad = CheckTreeIsPlain(root, e.path()); !bad.empty()) return bad;
    } else if (S_ISREG(st.st_mode)) {
      if (st.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) chmod(e.path().c_str(), st.st_mode & 0777);
    } else {
      return "the update has a special file";
    }
  }
  return ec ? "can't read the unpacked update" : std::string();
}

// What a crashed swap can leave next to the install: a half-built folder, and (only if it died in the instant between
// moving the player's folders and the exchange) those folders inside it. Puts them back, then removes the stage when
// it's old enough. A stage whose owner process is still alive is never touched.
void RecoverTarballStages(const fs::path& install, const fs::file_time_type& cutoff) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(install.parent_path(), ec)) {
    const std::string name = e.path().filename().string();
    if (!name.starts_with(kTarballStagePrefix)) continue;
    struct stat st {};
    if (lstat(e.path().c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) continue;
    std::ifstream journal(e.path() / "carried");
    long pid = 0;
    std::string top;
    if (journal && journal >> pid && pid > 0 && kill(pid_t(pid), 0) == 0) continue;  // mid-update right now
    if (journal) {
      std::getline(journal, top);  // rest of the pid line
      std::getline(journal, top);
      std::string item;
      while (std::getline(journal, item)) {
        if (item.empty() || item.find('/') != std::string::npos || item == "." || item == "..") continue;
        std::error_code r;
        if (fs::symlink_status(e.path() / top / item, r).type() != fs::file_type::not_found &&
            fs::symlink_status(install / item, r).type() == fs::file_type::not_found)
          fs::rename(e.path() / top / item, install / item, r);
      }
    }
    std::error_code t;
    if (fs::last_write_time(e.path(), t) < cutoff && !t) fs::remove_all(e.path(), t);
  }
}

// Builds the new folder from `tarball`, carries over what the player keeps, swaps it in. On success `old_folder` is
// where the previous install is now (to be deleted once the new one runs).
bool InstallTarball(const fs::path& install, const fs::path& tarball, const fs::path& tmp, const std::string& exe_name,
                    fs::path& old_folder, std::string& error) {
  std::error_code ec;
  const fs::path parent = install.parent_path();
  struct stat inst_st {};
  if (lstat(install.c_str(), &inst_st) != 0 || !S_ISDIR(inst_st.st_mode)) {
    error = "the install folder is missing";
    return false;
  }
  // Look at every member before anything is unpacked.
  std::string top;
  {
    const fs::path names = tmp / "names.txt", verbose = tmp / "verbose.txt";
    // -P: list the names as stored (tar would quietly drop a leading "/" or "../" and hide what the package asked for).
    if (Run({Tool("tar"), "-tJPf", tarball.string()}, names) != 0 || Run({Tool("tar"), "-tvJPf", tarball.string()}, verbose) != 0) {
      error = "can't read the downloaded package (is tar with xz support installed?)";
      return false;
    }
    error = CheckTarMembers(ReadSmallFile(names), ReadSmallFile(verbose), &top);
    if (!error.empty()) return false;
  }
  std::string stage_error;
  const fs::path stage = MakeUniqueDir(parent, kTarballStagePrefix, stage_error);
  if (stage.empty()) {
    error = stage_error;
    return false;
  }
  const fs::path fresh = stage / top;
  auto drop = [&] {
    std::error_code e;
    fs::remove_all(stage, e);
  };
  if (Run({Tool("tar"), "-xJf", tarball.string(), "-C", stage.string(), "--no-same-owner"}) != 0) {
    error = "can't unpack the update";
    drop();
    return false;
  }
  struct stat fresh_st {};
  if (lstat(fresh.c_str(), &fresh_st) != 0 || !S_ISDIR(fresh_st.st_mode)) {
    error = "the update isn't a folder";
    drop();
    return false;
  }
  if (std::string bad = CheckTreeIsPlain(fresh, fresh); !bad.empty()) {
    error = bad;
    drop();
    return false;
  }
  struct stat exe_st {};
  if (lstat((fresh / exe_name).c_str(), &exe_st) != 0 || !S_ISREG(exe_st.st_mode) || !(exe_st.st_mode & S_IXUSR)) {
    error = "the update has no " + exe_name + " program";
    drop();
    return false;
  }
  if (!HasTarballMarker(fresh)) {
    error = "the update isn't a tarball package";
    drop();
    return false;
  }
  chmod(fresh.c_str(), inst_st.st_mode & 07777);
  { int fd = open(stage.c_str(), O_RDONLY | O_DIRECTORY); if (fd >= 0) { syncfs(fd); close(fd); } }  // all new files on disk first
  TestHold("extracted");

  // Everything at the top of the old folder that the update doesn't ship stays with the player: game/ (the game files,
  // when kept here), logs/, anything they added. Moved into the new folder (a rename, so a link or a mount-less folder
  // keeps its identity), undone on any failure. A journal in the stage lets the next start put them back after a crash.
  std::vector<std::string> carry;
  for (const auto& e : fs::directory_iterator(install, ec)) {
    const std::string name = e.path().filename().string();
    if (fs::symlink_status(fresh / name, ec).type() == fs::file_type::not_found) carry.push_back(name);
  }
  {
    std::ofstream journal(stage / "carried");
    journal << getpid() << "\n" << top << "\n";
    for (const auto& n : carry) journal << n << "\n";
  }
  std::vector<std::string> moved;
  auto undo = [&] {
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
      std::error_code e;
      fs::rename(fresh / *it, install / *it, e);
    }
    moved.clear();
  };
  for (const auto& name : carry) {
    fs::rename(install / name, fresh / name, ec);
    if (ec) {
      error = "can't keep " + name + " in the new folder: " + Err(ec);
      undo();
      drop();
      return false;
    }
    moved.push_back(name);
  }
  TestHold("carried");
  if (int rc = ExchangeDirs(fresh, install); rc != 0) {
    error = rc == ENOSYS || rc == EINVAL ? "this file system can't swap folders in one step"
                                         : std::string("can't swap in the new folder: ") + std::strerror(rc);
    undo();
    drop();
    return false;
  }
  // `fresh` now holds the old install, and the install path the new one.
  fs::remove(stage / "carried", ec);
  TestHold("exchanged");
  old_folder = stage;
  return true;
}
#endif
#endif

// The AppImage file this process runs from, or empty. $APPIMAGE is only believed when this executable sits under
// $APPDIR: both variables are inherited by anything an AppImage starts, so a game or tool launched from one would
// otherwise think it was that AppImage.
fs::path OwnedAppImage() {
#if defined(_WIN32) || defined(__APPLE__)
  return {};
#else
  const char* image = std::getenv("APPIMAGE");
  const char* appdir = std::getenv("APPDIR");
  if (!image || !*image || !appdir || !*appdir) return {};
  std::error_code ec;
  fs::path exe = SelfExe(), dir = fs::canonical(appdir, ec);
  if (ec || exe.empty() || dir.empty()) return {};
  fs::path rel = exe.lexically_relative(dir);
  if (rel.empty() || *rel.begin() == "..") return {};
  fs::path img = fs::weakly_canonical(image, ec);
  if (ec || !img.is_absolute() || !fs::is_regular_file(img, ec)) return {};
  return img;
#endif
}

}  // namespace

bool UpdateSwapInProgress() { return g_swap_in_progress.load(); }

InstallKind CurrentInstallKind() {
  fs::path exe = SelfExe();
  const std::string dir = exe.empty() ? std::string() : PathToUtf8(exe.parent_path());
  bool setup_install = false, tarball = false;
#if defined(_WIN32)
  setup_install = WindowsIsSetupInstall(exe.parent_path());
#elif defined(__linux__)
  tarball = HasTarballMarker(exe.parent_path());
#endif
  return InstallKindFor(kOs, dir, OwnedAppImage().string(), setup_install, tarball);
}

fs::path SelfExeFolder() { return SelfExe().parent_path(); }

fs::path InstallTarget(InstallKind kind) {
  fs::path exe = SelfExe();
  switch (kind) {
    case InstallKind::kMacBundle: return exe.parent_path().parent_path().parent_path();
    case InstallKind::kLinuxAppImage: return OwnedAppImage();
    case InstallKind::kLinuxTarball: return exe.parent_path();
    case InstallKind::kNone: return {};
    default: return exe.parent_path();
  }
}

std::string SelfUpdateBlockerAt(InstallKind kind, const fs::path& target) {
  if (kind == InstallKind::kNone || target.empty()) return "this install can't update itself";
  std::error_code ec;
#if defined(__linux__)
  if (kind == InstallKind::kLinuxTarball) {
    // The folder itself (the player's files move around in it) and its parent (the new folder is built there, and the
    // two trade places) must be writable, and the file system must be able to trade them.
    if (!target.has_parent_path() || target == target.root_path()) return "the install folder has no parent folder";
    if (!Writable(target)) return target.string() + " isn't writable";
    if (!Writable(target.parent_path())) return target.parent_path().string() + " isn't writable";
    return CheckExchange(target.parent_path());
  }
#endif
  fs::path folder = (kind == InstallKind::kMacBundle || kind == InstallKind::kLinuxAppImage) ? target.parent_path() : target;
  if (!Writable(folder)) return PathToUtf8(folder) + " isn't writable";
  if (kind == InstallKind::kLinuxAppImage && !fs::is_regular_file(target, ec)) return "the AppImage file is missing";
  return {};
}

std::string SelfUpdateBlocker(InstallKind kind) { return SelfUpdateBlockerAt(kind, InstallTarget(kind)); }

bool InstallUpdate(InstallKind kind, const Asset& asset, InstallProgress& progress, std::string& error) {
  return InstallUpdateAt(kind, InstallTarget(kind), asset, progress, error, AppVersion());
}

bool InstallUpdateAt(InstallKind kind, const fs::path& target, const Asset& asset, InstallProgress& progress,
                     std::string& error, const std::string& running_version) {
  // Only a release whose listing verified against the build's key, and only forward.
  if (!asset.signed_ok) {
    error = "the release listing isn't signed";
    return false;
  }
  if (!IsNewerVersion(asset.version, running_version)) {
    error = "that release isn't newer than this one";
    return false;
  }
  if (asset.file.empty() || asset.file.find_first_of("/\\") != std::string::npos || asset.file[0] == '.') {
    error = "bad package name";
    return false;
  }
  error = SelfUpdateBlockerAt(kind, target);
  if (!error.empty()) return false;
  std::error_code ec;
  fs::path base = fs::temp_directory_path(ec);
  if (ec) {
    error = "no temp folder";
    return false;
  }
  fs::path tmp = MakeUniqueDir(base, kStagePrefix, error);
  if (tmp.empty()) return false;
  auto fail = [&](const std::string& why) {
    error = why;
    SetStage(progress, UpdateStage::kIdle);
    fs::remove_all(tmp, ec);
    return false;
  };

#if defined(__linux__)
  if (kind == InstallKind::kLinuxTarball) {
    // The download goes in the temp folder, the unpacked copy next to the install (several times the download).
    const uint64_t need_tmp = asset.size + (64ull << 20), need_here = asset.size * 8 + (200ull << 20);
    if (FreeBytes(tmp) < need_tmp) return fail("not enough free disk space in the temp folder (" + std::to_string(need_tmp >> 20) + " MB needed)");
    if (FreeBytes(target.parent_path()) < need_here)
      return fail("not enough free disk space next to the install (" + std::to_string(need_here >> 20) + " MB needed)");
  }
#endif
  SetStage(progress, UpdateStage::kDownloading);
  const fs::path file = tmp / asset.file;
  if (std::string e = Download(asset, file, progress); !e.empty()) return fail(e);

  SetStage(progress, UpdateStage::kVerifying);
  if (std::string e = VerifyDownload(file, asset); !e.empty()) return fail(e);

  SwapGuard swap;  // the window can't be closed from here until the files are in place
  SetStage(progress, UpdateStage::kInstalling);
  std::string e;
  bool ok = false;
#if defined(_WIN32)
  if (kind == InstallKind::kWindowsSetup) ok = InstallWindows(target, file, asset, e);
  // The temp folder stays: the setup program runs from it. CleanUpOldUpdates removes it on a later start.
  if (!ok) return fail(e.empty() ? "this install can't update itself" : e);
  SetStage(progress, UpdateStage::kRestarting);
  return true;
#else
  fs::path old_folder;
  std::string exe_name = "bots";
#if defined(__linux__)
  if (fs::path n = SelfExe().filename(); !n.empty()) exe_name = n.string();
#endif
  switch (kind) {
    case InstallKind::kMacBundle: ok = InstallMac(target, file, tmp, e); break;
    case InstallKind::kLinuxAppImage: ok = InstallAppImage(target, file, e); break;
#if defined(__linux__)
    case InstallKind::kLinuxTarball: ok = InstallTarball(target, file, tmp, exe_name, old_folder, e); break;
#endif
    default: e = "this install can't update itself";
  }
  if (!ok) return fail(e);
  SetStage(progress, UpdateStage::kRestarting);
  fs::remove_all(tmp, ec);
  if (kind == InstallKind::kMacBundle) RelaunchAfterExit({Tool("open"), "-n", target.string()});
  else if (kind == InstallKind::kLinuxTarball) RelaunchAfterExit({(target / exe_name).string()}, target, old_folder);
  else RelaunchAfterExit({target.string()});
  return true;
#endif
}

std::string TakeFailedUpdateNote() {
#if defined(_WIN32)
  const fs::path marker = PendingMarker();
  std::error_code ec;
  if (marker.empty() || !fs::exists(marker, ec)) return {};
  std::string version;
  std::getline(std::ifstream(marker), version);
  fs::remove(marker, ec);
  if (version == AppVersion()) return {};  // it did finish
  return "The last update" + (version.empty() ? std::string() : " (to " + version + ")") +
         " didn't finish. Your install should be as it was; \"Download it\" gets the new version by hand.";
#else
  return {};
#endif
}

void CleanUpOldUpdates() {
  std::error_code ec;
  const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(1);
  // Downloads from earlier updates (this process's own are removed when it finishes), only ones that are our own
  // folders and not recent: another copy of the launcher may be mid-update.
  fs::path base = fs::temp_directory_path(ec);
  if (ec) return;
  for (const auto& e : fs::directory_iterator(base, ec)) {
    const std::string name = PathToUtf8(e.path().filename());
    if (!name.starts_with(kStagePrefix)) continue;
#if !defined(_WIN32)
    struct stat st {};
    if (lstat(e.path().c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) continue;
#endif
    std::error_code t;
    if (fs::last_write_time(e.path(), t) < cutoff && !t) fs::remove_all(e.path(), t);
  }
#if !defined(_WIN32)
  // What a swap left next to the running program when something held it open.
  const InstallKind kind = CurrentInstallKind();
  fs::path target = InstallTarget(kind);
  if (target.empty()) return;
#if defined(__linux__)
  if (kind == InstallKind::kLinuxTarball) {
    RecoverTarballStages(target, cutoff);
    return;
  }
#endif
  for (const auto& e : fs::directory_iterator(target.parent_path(), ec)) {
    const std::string name = e.path().filename().string();
    if ((name.starts_with(kSwapPrefix) || name.starts_with(".skullburglar-old-")) && fs::last_write_time(e.path(), ec) < cutoff)
      fs::remove_all(e.path(), ec);
  }
#endif
}

}  // namespace bots::setup
