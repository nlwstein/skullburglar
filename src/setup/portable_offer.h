// The portable-mode offer (B-408). On a first start with no existing install, when the program runs from removable or
// external storage (an SD card, a USB drive), the launcher offers to keep the game files, saves and settings next to
// the game on that drive. Yes creates portable.txt there and restarts into portable mode (portable.h, B-400); No keeps
// the normal folders and the question is never asked again. Installs on internal storage are never asked.
//
// Everything here is pure (facts in, decision out) except the probes at the end, which fill the facts in per platform.
// tests/portable_offer_test.cpp covers the decision and the answer files on any host.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../path_utf8.h"
#include "portable.h"

namespace bots::setup {

// What a probe learned about the volume holding the program. Unknown facts keep the conservative default (no offer).
struct StorageFacts {
  bool under_media_mount = false;  // Linux: the path is under /run/media or /media, where the desktop mounts drives
  bool device_removable = false;   // the system says the device is removable (Linux sysfs, macOS, Windows drive type/bus)
  bool device_internal = false;    // macOS: the device is a built-in disk and not removable media
  bool same_as_home_or_boot = true;  // the same volume as the user's home folder or the system drive
  bool read_only = true;           // the volume can't be written (a mounted disk image, a locked card)
  bool network = false;            // a network share
};

// Conservative: removable or external, writable, not the volume the player's home or the system lives on.
inline bool IsRemovableOrExternal(const StorageFacts& f) {
  if (f.read_only || f.network || f.same_as_home_or_boot) return false;
  if (f.device_internal && !f.device_removable) return false;
  return f.under_media_mount || f.device_removable;
}

// What is already on the machine in the default places. Any of it means this is not a first start.
struct ExistingInstall {
  bool shared_game_files = false;  // the per-user game folder holds the game (app_paths.h DefaultGameDir)
  bool user_data = false;          // the default saves and settings folder has anything in it
  bool game_next_to_program = false;  // a game folder next to the program holds the game (B-400's 1.12 layout)
  bool settings_next_to_program = false;  // a bots.toml next to the executable (dev layout)
  bool Any() const { return shared_game_files || user_data || game_next_to_program || settings_next_to_program; }
};

struct OfferInput {
  bool portable_already = false;  // portable.txt or BOTS_DATA_DIR is in effect
  bool answered = false;          // the answer file is next to the program
  bool removable_or_external = false;
  ExistingInstall existing;
  bool launcher_visible = true;   // there is a launcher to show it in
  bool game_dir_given = false;    // --game_data_root on the command line: a test or a dev launch, not a player's first start
  bool disabled_by_env = false;   // BOTS_PORTABLE_OFFER=0
};

enum class OfferDecision {
  kShow,
  kAlreadyPortable,
  kAlreadyAnswered,
  kInternalStorage,
  kExistingInstall,
  kNoLauncher,
  kCommandLine,
  kDisabled,
};

inline OfferDecision DecideOffer(const OfferInput& in) {
  if (in.disabled_by_env) return OfferDecision::kDisabled;
  if (in.portable_already) return OfferDecision::kAlreadyPortable;
  if (in.answered) return OfferDecision::kAlreadyAnswered;
  if (in.game_dir_given) return OfferDecision::kCommandLine;
  if (!in.launcher_visible) return OfferDecision::kNoLauncher;
  if (!in.removable_or_external) return OfferDecision::kInternalStorage;
  if (in.existing.Any()) return OfferDecision::kExistingInstall;
  return OfferDecision::kShow;
}

inline const char* WhyNoOffer(OfferDecision d) {
  switch (d) {
    case OfferDecision::kShow: return "offered";
    case OfferDecision::kAlreadyPortable: return "portable mode is already on";
    case OfferDecision::kAlreadyAnswered: return "answered before";
    case OfferDecision::kInternalStorage: return "not on removable or external storage";
    case OfferDecision::kExistingInstall: return "an existing install is on this computer";
    case OfferDecision::kNoLauncher: return "no launcher";
    case OfferDecision::kCommandLine: return "game folder given on the command line";
    case OfferDecision::kDisabled: return "switched off (BOTS_PORTABLE_OFFER=0)";
  }
  return "";
}

// The answer is remembered next to the program, in the same folder as portable.txt. "Yes" is portable.txt itself;
// "No" is this file.
inline constexpr const char* kOfferDeclinedFile = "portable-declined.txt";

inline std::string DeclinedFileText() {
  return "You chose not to keep Skullburglar's files next to the game on this drive, so the game uses the normal folders "
         "on this computer and doesn't ask again.\n\nTo change your mind, delete this file and start the game again, "
         "or create an empty file named portable.txt next to the game to keep everything here.\n";
}

inline std::string MarkerFileText() {
  return "This file switches on portable mode: Skullburglar keeps its saves, settings, game files, shader cache and logs "
         "in the bots-data folder next to it, so the whole game moves with this folder.\n\nDelete this file to use the "
         "normal folders on this computer again.\n";
}

// Yes: creates portable.txt in `base`. Empty when it worked, else a sentence for the player. Removes a declined note,
// so the two files never disagree.
inline std::string AnswerYes(const std::filesystem::path& base) {
  std::error_code ec;
  if (std::FILE* f = bots::OpenFile(base / kPortableMarker, "wb")) {
    const std::string text = MarkerFileText();
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok) return "Can't write " + bots::PlainPathUtf8(base / kPortableMarker) + ".";
    std::filesystem::remove(base / kOfferDeclinedFile, ec);
    return {};
  }
  return "Can't create " + bots::PlainPathUtf8(base / kPortableMarker) + ". Is the drive locked?";
}

// No: remembers it. A drive that can't be written is the reason to ask, not to nag: the failure is returned, the
// caller logs it, and the offer simply comes back on the next start.
inline std::string AnswerNo(const std::filesystem::path& base) {
  if (std::FILE* f = bots::OpenFile(base / kOfferDeclinedFile, "wb")) {
    const std::string text = DeclinedFileText();
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return {};
  }
  return "Can't write " + bots::PlainPathUtf8(base / kOfferDeclinedFile) + ".";
}

inline bool IsAnswered(const std::filesystem::path& base) {
  std::error_code ec;
  return std::filesystem::is_regular_file(base / kOfferDeclinedFile, ec);
}

// Linux: is `real_path` (already resolved through links) under a mount point where desktops put drives?
inline bool UnderMediaMount(const std::filesystem::path& real_path) {
  const std::string p = bots::PathUtf8(real_path.lexically_normal());
  return p.rfind("/run/media/", 0) == 0 || p.rfind("/media/", 0) == 0;
}

// Probes (src/setup/portable_offer.cpp): facts about the volume holding `dir`.
StorageFacts ProbeStorage(const std::filesystem::path& dir);

// Where the saves and settings go without portable mode: the SDK's user folder (Documents on Windows, $XDG_DATA_HOME or
// ~/.local/share elsewhere) joined with "bots". Empty when it can't be told.
std::filesystem::path DefaultUserDataDir();

// Probe: what the default places hold. `next_to_program` is the portable game folder (PortableGameDir), `exe_dir`
// the folder with the executable, `shared_game`/`user_data` the default folders (app_paths.h; the saves folder is the
// SDK's user folder joined with "bots").
ExistingInstall ProbeExistingInstall(const std::filesystem::path& shared_game, const std::filesystem::path& user_data,
                                     const std::filesystem::path& next_to_program,
                                     const std::filesystem::path& exe_dir);

}  // namespace bots::setup
