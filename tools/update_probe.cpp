// Test harness for the launcher's self-update, without the game: the same check and install code, run from a stand-in
// app (a macOS bundle, an AppImage, an unpacked tarball folder, a Windows install folder) whose only job is to update
// itself. tools/update_probe_test.sh builds it twice (old and new version), serves a fake releases file and runs it.
//   update_probe           open: records its version in $PROBE_MARKER (the "game started" sign) and exits
//   update_probe update    check the releases file, install a newer version if there is one, print what happened
//   update_probe clean     run the start-up clean-up (leftovers of an update that was killed), as the launcher does
//   update_probe /UPDATE ... (Windows) stand-in for the setup program, see below
//   update_probe checklist <names file> <verbose file>   run the tarball member check on two `tar -t` listings
// Built with the same defines as a release (BOTS_VERSION, BOTS_UPDATE_URL, BOTS_UPDATE_PUBKEY), plus
// BOTS_ALLOW_HTTP_UPDATE_URL for the local test server (tools/update_probe_test.sh makes a throwaway key):
//   c++ -std=c++20 -I src -DBOTS_VERSION='"0.0.1"' -DBOTS_UPDATE_URL='"http://127.0.0.1:8765/version.json"' \
//       -DBOTS_UPDATE_PUBKEY='"<64 hex>"' -DBOTS_ALLOW_HTTP_UPDATE_URL=1 -pthread [-framework CoreFoundation on macOS] \
//       tools/update_probe.cpp src/setup/update_check.cpp src/setup/update_logic.cpp src/setup/ed25519.cpp src/setup/update_install.cpp
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include <iterator>

#include "setup/update_check.h"
#include "setup/update_install.h"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace bots::setup;

#if defined(_WIN32)
// A stand-in for the setup program, for a test on a machine whose real install and registry must stay alone: the update
// downloads this very program as its "setup" and runs it as `<setup> /S /UPDATE /D=<folder>`. Like the real one it waits
// for the running copy to quit, puts itself over <folder>\bots.exe, deletes the pending marker, and starts the program.
static int FakeSetup(int argc, char** argv) {
  std::string dir;
  for (int i = 1; i < argc; ++i)
    if (!std::strncmp(argv[i], "/D=", 3)) dir = argv[i] + 3;
  wchar_t self[32768];
  GetModuleFileNameW(nullptr, self, 32768);
  const std::wstring exe = std::wstring(dir.begin(), dir.end()) + L"\\bots.exe";
  bool copied = false;
  for (int i = 0; i < 100 && !copied; ++i) {
    copied = CopyFileW(self, exe.c_str(), FALSE) != 0;
    if (!copied) Sleep(300);
  }
  if (!copied) return 1;
  if (const char* la = std::getenv("LOCALAPPDATA")) DeleteFileA((std::string(la) + "\\bots-recomp\\update-pending.txt").c_str());
  std::wstring cmd = L"\"" + exe + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const std::wstring wdir(dir.begin(), dir.end());  // like the real setup, which ends up with the install folder as its working folder
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, wdir.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  return 0;
}
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "/UPDATE")) return FakeSetup(argc, argv);
#endif
  bool update = false;  // an AppImage's AppRun adds its own arguments, so look at all of them
  for (int i = 1; i < argc; ++i) {
    update = update || !std::strcmp(argv[i], "update");
    // hold: keep running (and the executable locked) for a minute, so a setup program waiting on it times out.
    if (!std::strcmp(argv[i], "hold")) {
      std::this_thread::sleep_for(std::chrono::seconds(60));
      return 0;
    }
    // clean: what the launcher does on every start, run on its own: remove update leftovers from the temp folder.
    if (!std::strcmp(argv[i], "clean")) {
      CleanUpOldUpdates();
      std::printf("cleaned\n");
      return 0;
    }
    if (!std::strcmp(argv[i], "checklist") && i + 2 < argc) {
      auto slurp = [](const char* f) {
        std::ifstream in(f, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      };
      std::string top, why = CheckTarMembers(slurp(argv[i + 1]), slurp(argv[i + 2]), &top);
      std::printf("%s\n", why.empty() ? ("ok, top folder " + top).c_str() : ("refused: " + why).c_str());
      return why.empty() ? 0 : 1;
    }
    // note: print what the launcher would say about an update that never finished.
    if (!std::strcmp(argv[i], "note")) {
      std::printf("note: %s\n", TakeFailedUpdateNote().c_str());
      return 0;
    }
  }
  if (const char* marker = std::getenv("PROBE_MARKER"); marker && *marker && !update) {
    // PROBE_CLEAN_ON_START=1: do what the launcher does as it starts, before it checks for updates.
    if (const char* c = std::getenv("PROBE_CLEAN_ON_START"); c && *c == '1') {
      CleanUpOldUpdates();
      TakeFailedUpdateNote();
    }
    std::ofstream(marker, std::ios::app) << "started " << AppVersion() << " ran as " << argv[0] << ", install at " << PathToUtf8(SelfExeFolder()) << " (install kind "
                                         << int(CurrentInstallKind()) << ")\n";
    return 0;
  }
  if (!update) {
    std::printf("usage: update_probe update\n");
    return 2;
  }
  std::printf("version %s, update check %s, signing key %s\n", AppVersion(), UpdateCheckAvailable() ? "on" : "off",
              SelfUpdateKeyConfigured() ? "set" : "missing");
  UpdateInfo info = FetchUpdateInfo();
  if (!info.error.empty()) {
    std::printf("check failed: %s\n", info.error.c_str());
    return 1;
  }
  std::printf("newest %s, listing %s\n", info.latest.c_str(), info.signed_ok ? "signed" : "NOT signed");
  // PROBE_FORCE=1 skips this launcher-level check, to show the installer refuses an old version by itself.
  if (!std::getenv("PROBE_FORCE") && !IsNewerVersion(info.latest, AppVersion())) {
    std::printf("up to date\n");
    return 0;
  }
  InstallKind kind = CurrentInstallKind();
  // PROBE_WIN_TARGET=<folder> (Windows): treat that folder as a setup-made install without the registry entry the real
  // setup writes, so a test never has to touch the machine's registry.
  std::string win_target;
#if defined(_WIN32)
  if (const char* t = std::getenv("PROBE_WIN_TARGET"); t && *t) {
    kind = InstallKind::kWindowsSetup;
    win_target = t;
  }
#endif
  const Asset* asset = PickAsset(info, kind);
  std::printf("install kind %d, package %s\n", int(kind), asset ? asset->file.c_str() : "(none)");
  if (!asset) return 1;
  std::string blocker = win_target.empty() ? SelfUpdateBlocker(kind) : SelfUpdateBlockerAt(kind, win_target);
  if (!blocker.empty()) {
    std::printf("blocked: %s\n", blocker.c_str());
    return 1;
  }
  InstallProgress progress;
  std::string error;
  bool ok = false;
  std::atomic<bool> done{false};
  std::thread worker([&] {
    ok = win_target.empty() ? InstallUpdate(kind, *asset, progress, error)
                            : InstallUpdateAt(kind, win_target, *asset, progress, error, AppVersion());
    done = true;
  });
  int last = -1;
  while (!done) {
    int s = progress.stage.load();
    if (s != last) std::printf("stage %d\n", last = s);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  worker.join();
  if (!ok) {
    std::printf("update failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("updated; restarting (downloaded %llu of %llu bytes)\n", (unsigned long long)progress.done.load(),
              (unsigned long long)progress.total.load());
  return 0;
}
