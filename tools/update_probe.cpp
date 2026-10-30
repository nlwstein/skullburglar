// Test harness for the launcher's self-update, without the game: the same check and install code, run from a stand-in
// app (a macOS bundle, an AppImage, an unpacked tarball folder, a Windows install folder) whose only job is to update
// itself. tools/update_probe_test.sh builds it twice (old and new version), serves a fake releases file and runs it.
//   update_probe           open: records its version in $PROBE_MARKER (the "game started" sign) and exits
//   update_probe clean     run CleanUpOldUpdates() (the temp-folder sweep the launcher does at start)
//   update_probe update    check the releases file, install a newer version if there is one, print what happened
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

#include "setup/update_check.h"
#include "setup/update_install.h"

using namespace bots::setup;

int main(int argc, char** argv) {
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
    // note: print what the launcher would say about an update that never finished.
    if (!std::strcmp(argv[i], "note")) {
      std::printf("note: %s\n", TakeFailedUpdateNote().c_str());
      return 0;
    }
  }
  if (const char* marker = std::getenv("PROBE_MARKER"); marker && *marker && !update) {
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
  const Asset* asset = PickAsset(info, kind);
  std::printf("install kind %d, package %s\n", int(kind), asset ? asset->file.c_str() : "(none)");
  if (!asset) return 1;
  std::string blocker = SelfUpdateBlocker(kind);
  if (!blocker.empty()) {
    std::printf("blocked: %s\n", blocker.c_str());
    return 1;
  }
  InstallProgress progress;
  std::string error;
  bool ok = false;
  std::atomic<bool> done{false};
  std::thread worker([&] {
    ok = InstallUpdate(kind, *asset, progress, error);
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
