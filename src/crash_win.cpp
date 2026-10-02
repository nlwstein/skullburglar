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

namespace {

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

void WriteMinidump(EXCEPTION_POINTERS* ep) {
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
    return;
  }
  char path[64];
  std::snprintf(path, sizeof(path), "logs\\crash-%lu.dmp", GetCurrentProcessId());
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "minidump: can't create %s\n", path);
    return;
  }
  MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
  // Not full memory: the guest address space alone is several GB of reservations.
  auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                            MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
  BOOL ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr,
                       nullptr);
  CloseHandle(file);
  std::fprintf(stderr, "minidump: %s %s\n", path, ok ? "written" : "FAILED");
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
  WriteMinidump(ep);
  std::fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallWindowsCrashLogger() { SetUnhandledExceptionFilter(UnhandledFilter); }

#endif
