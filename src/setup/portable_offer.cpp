// Probes for the portable-mode offer (portable_offer.h, B-408): which kind of storage the program runs from, and
// whether an install already exists. Read-only: nothing is created or written here.
#include "portable_offer.h"

#include <cstdlib>
#include <cstring>

#include "app_paths.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <DiskArbitration/DiskArbitration.h>
#endif
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#endif

namespace bots::setup {

namespace fs = std::filesystem;

namespace {

#if !defined(_WIN32)
bool SameDevice(const struct stat& a, const fs::path& other) {
  struct stat b{};
  return !other.empty() && stat(other.c_str(), &b) == 0 && a.st_dev == b.st_dev;
}
#endif

#if defined(__APPLE__) && !TARGET_OS_IPHONE
// DiskArbitration: is the device behind this mount a built-in disk, and is the media removable or ejectable? A mounted
// disk image reports as external.
void ProbeMacDisk(const char* from_name, StorageFacts& f) {
  if (!from_name || std::strncmp(from_name, "/dev/", 5) != 0) return;
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  if (!session) return;
  if (DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, from_name + 5)) {
    if (CFDictionaryRef d = DADiskCopyDescription(disk)) {
      auto flag = [&](CFStringRef key) {
        const void* v = CFDictionaryGetValue(d, key);
        return v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(v));
      };
      const bool removable = flag(kDADiskDescriptionMediaRemovableKey) || flag(kDADiskDescriptionMediaEjectableKey);
      f.device_removable = removable || !flag(kDADiskDescriptionDeviceInternalKey);
      f.device_internal = flag(kDADiskDescriptionDeviceInternalKey);
      CFRelease(d);
    }
    CFRelease(disk);
  }
  CFRelease(session);
}
#endif

}  // namespace

StorageFacts ProbeStorage(const fs::path& dir) {
  StorageFacts f;
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
  (void)dir;  // phones: no launcher, no offer
  return f;
#else
  std::error_code ec;
  const fs::path real = fs::canonical(dir, ec);
  if (ec) return f;
#if defined(_WIN32)
  wchar_t vol[MAX_PATH] = {};
  if (!GetVolumePathNameW(real.c_str(), vol, MAX_PATH)) return f;
  const UINT type = GetDriveTypeW(vol);
  f.network = type == DRIVE_REMOTE;
  wchar_t sys[MAX_PATH] = {};
  wchar_t profile[MAX_PATH] = {};
  wchar_t sysvol[MAX_PATH] = {}, profvol[MAX_PATH] = {};
  GetSystemDirectoryW(sys, MAX_PATH);
  GetVolumePathNameW(sys, sysvol, MAX_PATH);
  const DWORD np = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  if (np > 0 && np < MAX_PATH) GetVolumePathNameW(profile, profvol, MAX_PATH);
  f.same_as_home_or_boot = _wcsicmp(vol, sysvol) == 0 || (profvol[0] && _wcsicmp(vol, profvol) == 0);
  DWORD flags = 0;
  f.read_only = !GetVolumeInformationW(vol, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) || (flags & FILE_READ_ONLY_VOLUME);
  bool usb = false;
  {  // the bus the volume sits on: USB sticks and external drives are USB, an SD card reader often too
    wchar_t dev[8] = {L'\\', L'\\', L'.', L'\\', vol[0], L':', 0};
    HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      STORAGE_PROPERTY_QUERY q{};
      q.PropertyId = StorageDeviceProperty;
      q.QueryType = PropertyStandardQuery;
      alignas(8) char buf[1024] = {};
      DWORD got = 0;
      if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, nullptr) &&
          got >= sizeof(STORAGE_DESCRIPTOR_HEADER)) {
        const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf);
        usb = desc->BusType == BusTypeUsb || desc->BusType == BusTypeSd || desc->BusType == BusTypeMmc;
        if (desc->RemovableMedia) f.device_removable = true;
      }
      CloseHandle(h);
    }
  }
  f.device_removable = f.device_removable || type == DRIVE_REMOVABLE || usb;
  f.under_media_mount = f.device_removable;
#else
  struct stat st{};
  if (stat(real.c_str(), &st) != 0) return f;
  const fs::path home = bots::EnvPath("HOME");
  f.same_as_home_or_boot = SameDevice(st, "/") || SameDevice(st, home);
#if defined(__APPLE__)
  struct statfs sf{};
  if (statfs(real.c_str(), &sf) != 0) return f;
  f.read_only = (sf.f_flags & MNT_RDONLY) != 0;
  const std::string fstype = sf.f_fstypename;
  f.network = fstype == "smbfs" || fstype == "nfs" || fstype == "afpfs" || fstype == "webdav";
  f.under_media_mount = bots::PathUtf8(real).rfind("/Volumes/", 0) == 0;
#if !TARGET_OS_IPHONE
  ProbeMacDisk(sf.f_mntfromname, f);
#endif
  // Off the boot volume, under /Volumes, on a device that isn't built in (or is removable): external.
  f.under_media_mount = f.under_media_mount && f.device_removable;
#else
  struct statvfs sv{};
  if (statvfs(real.c_str(), &sv) != 0) return f;
  f.read_only = (sv.f_flag & ST_RDONLY) != 0;
  struct statfs sf{};
  if (statfs(real.c_str(), &sf) == 0) {
    const auto t = static_cast<unsigned long>(sf.f_type);
    f.network = t == 0x6969UL /*NFS*/ || t == 0x517BUL /*SMB*/ || t == 0xFF534D42UL /*CIFS*/;
  }
  f.under_media_mount = UnderMediaMount(real);
  // /sys/dev/block/<major>:<minor> is the partition (or disk); its own "removable" file, or its parent disk's.
  {
    const fs::path link = fs::path("/sys/dev/block") / (std::to_string(major(st.st_dev)) + ":" + std::to_string(minor(st.st_dev)));
    std::error_code e;
    const fs::path node = fs::canonical(link, e);
    if (!e) {
      for (const fs::path& p : {node / "removable", node.parent_path() / "removable"}) {
        if (std::FILE* fp = std::fopen(p.c_str(), "r")) {
          char c = 0;
          const bool got = std::fread(&c, 1, 1, fp) == 1;
          std::fclose(fp);
          if (got) {
            f.device_removable = c == '1';
            break;
          }
        }
      }
    }
  }
#endif
#endif
  return f;
#endif
}

fs::path DefaultUserDataDir() {
#if defined(_WIN32)
  PWSTR docs = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs))) {
    out = fs::path(docs) / "bots";
    CoTaskMemFree(docs);
  }
  return out;
#elif defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  return {};
#else
  if (const fs::path xdg = bots::EnvPath("XDG_DATA_HOME"); !xdg.empty()) return xdg / "bots";
  if (const fs::path home = bots::EnvPath("HOME"); !home.empty()) return home / ".local" / "share" / "bots";
  return {};
#endif
}

ExistingInstall ProbeExistingInstall(const fs::path& shared_game, const fs::path& user_data,
                                     const fs::path& next_to_program, const fs::path& exe_dir) {
  ExistingInstall e;
  std::error_code ec;
  e.shared_game_files = IsGameDir(shared_game);
  if (!user_data.empty() && fs::is_directory(user_data, ec)) e.user_data = !fs::is_empty(user_data, ec);
  e.game_next_to_program = IsGameDir(next_to_program);
  e.settings_next_to_program = !exe_dir.empty() && fs::is_regular_file(exe_dir / "bots.toml", ec);
  return e;
}

}  // namespace bots::setup
