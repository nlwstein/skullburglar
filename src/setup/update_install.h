// "Update now" for the launcher: downloads the release's package for this install, checks its size and SHA-256, swaps
// it in and starts the new version. See docs/plans/update-button.md in the project notes for the design; the short
// version, per install (update_logic.h's InstallKind):
//   Windows  runs the setup program silently over the install folder (it keeps saves and game files, waits for this
//            process to exit, starts the new version)
//   macOS    mounts the disk image, copies Skullburglar.app beside the running one, swaps the two, reopens it
//   Linux    AppImage: the new file replaces the running one (an atomic rename). An unpacked tarball doesn't update
//            itself (it has no record of which files are the program's): it keeps the "Download it" button.
// Nothing is installed unless the release listing carried a valid Ed25519 signature (update_logic.h) and the release is
// newer than the running version. The download is made by this program (WinHTTP, or curl on macOS and Linux), not by a browser, so the files carry
// no "downloaded from the internet" mark (macOS quarantine, Windows Mark of the Web) and the system raises no security
// prompt for the new version. Nothing outside the install is touched: saves, settings and game files live elsewhere.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "update_logic.h"

namespace bots::setup {

enum class UpdateStage { kIdle, kDownloading, kVerifying, kInstalling, kRestarting };

// Shared with the UI thread.
struct InstallProgress {
  std::atomic<int> stage{int(UpdateStage::kIdle)};
  std::atomic<uint64_t> done{0}, total{0};  // bytes, while downloading
  std::atomic<bool> cancel{false};          // honoured while downloading
};

// What the running program is installed as.
InstallKind CurrentInstallKind();

// The folder holding the running executable (empty when unknown).
std::filesystem::path SelfExeFolder();

// A path as UTF-8 text. On Windows path::string() goes through the ANSI code page and throws for a name it can't hold, so
// use this wherever a path becomes text (a message, a comparison).
std::string PathToUtf8(const std::filesystem::path& p);

// What InstallUpdateAt replaces: the app bundle (macOS), the AppImage file (Linux), else the folder holding the
// executable (Windows). Empty for kNone.
std::filesystem::path InstallTarget(InstallKind kind);

// Why this install can't update itself ("" when it can): not a release-type install, or its folder isn't writable.
// The launcher then keeps only the "Download it" button.
std::string SelfUpdateBlocker(InstallKind kind);
std::string SelfUpdateBlockerAt(InstallKind kind, const std::filesystem::path& target);

// True from the moment files are being replaced until the install has finished or failed. The window must not close
// then (bots_app.h refuses the close request), or an install could be left half done.
bool UpdateSwapInProgress();

// Windows: if the last update's setup program never finished (it leaves a marker until it does), says so once. Empty
// otherwise, and always empty elsewhere.
std::string TakeFailedUpdateNote();

// Downloads, verifies and installs `asset`, then starts the new version. True when that has happened: the caller must
// quit now (the new copy is waiting for this process to end). False with `error` set when nothing changed; a swap
// that fails half way is undone first.
bool InstallUpdate(InstallKind kind, const Asset& asset, InstallProgress& progress, std::string& error);

// The same for an explicit target and running version (tests point it at a scratch install). Refuses an asset that
// isn't signed_ok or whose version isn't newer than `running_version`.
bool InstallUpdateAt(InstallKind kind, const std::filesystem::path& target, const Asset& asset,
                     InstallProgress& progress, std::string& error, const std::string& running_version);

// Removes what an earlier update left behind (downloads in the temp folder, a replaced copy that was in use).
void CleanUpOldUpdates();

}  // namespace bots::setup
