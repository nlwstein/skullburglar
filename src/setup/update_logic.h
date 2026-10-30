// The pure half of the launcher's self-update (B-update): versions, the releases file, which download fits this
// install, SHA-256. No I/O beyond reading a file for the hash, so tests/update_test.cpp builds it alone.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bots::setup {

// True when `v` starts with a dotted number ("0.4.0"); "dev" doesn't.
bool HasVersionNumber(const std::string& v);

// True when version `a` is newer than `b` (dotted numbers; a suffix such as "-dev.abc" is ignored).
bool IsNewerVersion(const std::string& a, const std::string& b);

// One player package from the releases file (tools/ci/publish_downloads.py writes it).
struct Asset {
  std::string file;    // file name, safe to join to a folder
  std::string url;     // absolute address, on the same site as the releases file
  std::string sha256;  // 64 lowercase hex digits
  uint64_t size = 0;
  std::string version;    // the release this package belongs to
  bool signed_ok = false; // the release's signature checked out against the build's public key
};

struct UpdateInfo {
  std::string latest;  // newest released version, empty when the check failed
  std::string page;    // its download page
  std::string error;   // why the check failed, for the log
  bool signed_ok = false;  // the newest release's entry carries a valid signature (see ParseReleases)
  // The newest release's packages by the releases file's key: windows, macos, linux_appimage, linux_tar. Only entries
  // that passed ParseReleases' checks are here.
  std::map<std::string, Asset> downloads;
};

// Reads the releases file `body`, fetched from `base_url`. Fills latest, page and downloads (error when `latest` is
// missing). A download is kept only when its file name is a plain name, its sha256 is 64 hex digits, its size is
// between 1 byte and 2 GB and its address (relative ones resolve against base_url) is on base_url's own site, over
// https unless base_url itself is plain http (a local test server).
//
// "Update now" needs authenticity as well as integrity (the checksum comes from the same site as the file), so the
// newest release's entry must carry "signature": 128 hex digits, an Ed25519 signature of SigningMessage() under
// `public_key_hex` (64 hex digits, compiled into the build). Without a key, or with a signature that doesn't verify,
// signed_ok is false and the packages are listed but marked unsigned: the launcher then offers "Download it" only.
UpdateInfo ParseReleases(const std::string& body, const std::string& base_url, const std::string& public_key_hex = {});

// The text that is signed for a release: its version and, one line "key file size sha256\n" per package, the
// packages (sorted). tools/ci/publish_downloads.py builds the same text.
std::string SigningMessage(const std::string& version, std::vector<std::string> package_lines);

// What kind of install this program runs from; decides which package replaces it.
enum class InstallKind {
  kNone,          // a build run from a source tree, a disk image, a read-only place, a phone: no self-update
  kWindowsSetup,  // the per-user setup program's install folder: the setup program, run silently
  kMacBundle,     // Skullburglar.app: the disk image's app replaces the bundle
  kLinuxAppImage, // a running AppImage: the new file replaces it
  kLinuxTarball,  // an unpacked .tar.xz (it carries an install-kind marker): the new folder is swapped in as a whole
};

// `os` is "windows", "macos" or "linux". `exe_dir` is the folder holding the executable, `appimage` is the AppImage
// file this process runs from (empty unless it really is one: update_install.cpp checks $APPIMAGE against $APPDIR).
// `windows_setup_install` says the folder was made by the setup program (it has uninstall.exe and the registry entry
// naming it): an unzipped copy must never have the setup run over it. A macOS app must be a bundle (Contents/MacOS)
// that isn't on a disk image or in App Translocation's random folder. `linux_tarball_marker` says the folder holds the
// install-kind marker file that only the tarball ships (update_install.cpp checks it): a dev or build tree, or any
// folder without it, never updates.
InstallKind InstallKindFor(const std::string& os, const std::string& exe_dir, const std::string& appimage,
                           bool windows_setup_install = false, bool linux_tarball_marker = false);

// Checks the member list of a tarball before anything is unpacked. `names` is `tar -tJf` output (one name per line),
// `verbose` is `tar -tvJf` output for the same archive (same number of lines). Refuses absolute names, a ".."
// component, hard links, devices, pipes, symlinks that are absolute or contain "..", and more than one top-level
// folder. Empty on success (`top` then names the one top-level folder), else why not.
std::string CheckTarMembers(const std::string& names, const std::string& verbose, std::string* top = nullptr);

// macOS only: whether the app runs from a disk image or App Translocation's random folder, which is why it can't
// update itself; the launcher then says to move it to Applications first.
bool MacAppNeedsMove(const std::string& exe_dir);

// The key in UpdateInfo::downloads for an install kind; empty for kNone.
const char* AssetKey(InstallKind kind);

// The package that updates an install of `kind`, or null (kNone, or the release has none for it).
const Asset* PickAsset(const UpdateInfo& info, InstallKind kind);

// Streaming SHA-256.
class Sha256 {
 public:
  Sha256();
  void Update(const void* data, size_t len);
  std::string Final();  // lowercase hex; the object can't be used afterwards

 private:
  void Block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_ = 0;
  uint64_t total_ = 0;
};

std::string Sha256Hex(const std::string& data);
// Hex digest of a file; empty when it can't be read.
std::string Sha256File(const std::filesystem::path& path);

// The checks every download passes before anything is installed from it: exactly the listed size, and the listed hash.
// Empty on success, else why not.
std::string VerifyDownload(const std::filesystem::path& path, const Asset& asset);

}  // namespace bots::setup
