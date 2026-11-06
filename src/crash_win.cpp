// Windows counterpart of BotsApp::CrashHandler: on an unhandled exception, write the exception, the
// faulting address and an unwound host stack as module+offset lines to stderr (logs\run.out).
// Resolve with llvm-symbolizer against the build's PDBs.
// Also writes logs\crash-<pid>.dmp (minidump: threads, stacks, memory they reference) with
// dbghelp.dll, loaded at crash time so the exe doesn't link it.
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "path_utf8.h"
#include "save_backup.h"

namespace {

// Where a crash is archived (B-065; save_backup.h): set at startup by SetWindowsCrashArchive.
std::filesystem::path g_crash_dir;
std::filesystem::path g_crash_log;
std::string g_crash_stamp;
std::filesystem::path g_exe_folder;  // where logs\ is: next to bots.exe, whatever the working directory is

void PrintFrame(int i, DWORD64 pc) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &module)) {
    GetModuleFileNameA(module, name, sizeof(name));
  }
  const char* base = name;
  for (const char* p = name; *p; ++p) {
    if (*p == '\\' || *p == '/') base = p + 1;
  }
  std::fprintf(stderr, "  #%02d %s+0x%llx\n", i, base,
               static_cast<unsigned long long>(pc - reinterpret_cast<DWORD64>(module)));
}

// Returns the dump's path, or empty when it couldn't be written. The path is absolute and wide: it doesn't depend on
// the working directory, and an install folder with a non-ASCII name works.
std::filesystem::path WriteMinidump(EXCEPTION_POINTERS* ep) {
  using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                            PMINIDUMP_EXCEPTION_INFORMATION,
                                            PMINIDUMP_USER_STREAM_INFORMATION,
                                            PMINIDUMP_CALLBACK_INFORMATION);
  HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
  auto write_dump =
      dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
              : nullptr;
  if (!write_dump) {
    std::fprintf(stderr, "minidump: dbghelp.dll unavailable\n");
    return {};
  }
  const std::filesystem::path path = bots::savebackup::DumpPath(g_exe_folder, GetCurrentProcessId());
  const std::string shown = bots::PathUtf8(path);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "minidump: can't create %s\n", shown.c_str());
    return {};
  }
  MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
  // Not full memory: the guest address space alone is several GB of reservations.
  auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                            MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
  BOOL ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr,
                       nullptr);
  CloseHandle(file);
  std::fprintf(stderr, "minidump: %s %s\n", shown.c_str(), ok ? "written" : "FAILED");
  return ok ? path : std::filesystem::path();
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  std::fprintf(stderr, "\n=== bots crash: exception 0x%08lx at %p", er->ExceptionCode,
               er->ExceptionAddress);
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    std::fprintf(stderr, " (%s 0x%llx)", er->ExceptionInformation[0] ? "write" : "read",
                 static_cast<unsigned long long>(er->ExceptionInformation[1]));
  }
  std::fprintf(stderr, " ===\n");
  // PE link timestamp of bots.exe: archive each build (exe + PDB) under this stamp on the build
  // machine (e.g. ~/bots/win/archive/<stamp>/), so the frames below can be symbolized later.
  if (auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleA(nullptr))) {
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) +
                                                          dos->e_lfanew);
    std::fprintf(stderr, "build stamp %08lx\n", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
  }
  CONTEXT ctx = *ep->ContextRecord;
  for (int i = 0; i < 64 && ctx.Rip; ++i) {
    PrintFrame(i, ctx.Rip);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (fn) {
      void* handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                       &establisher_frame, nullptr);
    } else {
      // Leaf function: the return address is at the top of the stack.
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
    }
  }
  std::fflush(stderr);
  const std::filesystem::path dump = WriteMinidump(ep);
  // Keep the dump and this session's log under crashes\<stamp>, where the next launch won't rotate them away.
  if (!g_crash_dir.empty()) {
    std::vector<std::filesystem::path> files{g_crash_log};
    if (!dump.empty()) files.push_back(dump);
    const int n = bots::savebackup::ArchiveCrash(files, g_crash_dir, g_crash_stamp);
    std::fprintf(stderr, "crash archive: %d file(s) in crashes\\%s\n", n, g_crash_stamp.c_str());
  }
  std::fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void SetWindowsCrashArchive(const std::filesystem::path& dir, const std::string& stamp,
                            const std::filesystem::path& log) {
  g_crash_log = log;
  g_crash_stamp = stamp;
  g_crash_dir = dir;
}

void InstallWindowsCrashLogger() {
  // The executable's folder, read now: the crash handler shouldn't have to ask the system while the process is dying.
  wchar_t exe[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(sizeof(exe) / sizeof(exe[0])));
  if (n > 0 && n < sizeof(exe) / sizeof(exe[0])) g_exe_folder = std::filesystem::path(exe).parent_path();
  SetUnhandledExceptionFilter(UnhandledFilter);
}

#endif
