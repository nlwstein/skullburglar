// Standalone harness for src/crash_win.cpp: crashes on purpose so the minidump path and the crash archive (B-065) can be
// checked on a Windows machine without the whole game. Build (in the win-cross container, see tools/win-cross/build.sh):
//   clang++ --target=x86_64-pc-windows-msvc -std=c++20 -D_CRT_SECURE_NO_WARNINGS <the xwin -idirafter flags> -I src \
//     tools/win-cross/crash_harness.cpp src/crash_win.cpp -fuse-ld=lld <the xwin -L flags> -o crash_harness.exe
// Run from ANY folder:  crash_harness.exe <crash-archive-dir> <session-log-file> [stamp]
// Expect: <exe folder>\logs\crash-<pid>.dmp, and <crash-archive-dir>\<stamp>\ holding that dump and a copy of the log.
#if defined(_WIN32)
#include <cstdio>
#include <filesystem>
#include <string>

void InstallWindowsCrashLogger();
void SetWindowsCrashArchive(const std::filesystem::path& dir, const std::string& stamp, const std::filesystem::path& log);

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: crash_harness <crash-archive-dir> <session-log-file> [stamp]\n");
    return 2;
  }
  SetWindowsCrashArchive(argv[1], argc > 3 ? argv[3] : "20260101-000000", argv[2]);
  InstallWindowsCrashLogger();
  volatile int* p = nullptr;
  *p = 1;  // access violation
  return 0;
}
#endif
