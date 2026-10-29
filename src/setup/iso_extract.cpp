// See iso_extract.h.
#include "../path_utf8.h"
#include "iso_extract.h"
#include "game_swap.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/logging.h>

namespace bots::setup {
namespace fs = std::filesystem;
using rex::filesystem::DiscImageEntry;
using rex::filesystem::Entry;

namespace {

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool IsDirectory(const Entry* e) {
  return (e->attributes() & rex::filesystem::kFileAttributeDirectory) != 0;
}

void Count(const Entry* dir, uint64_t& bytes, uint32_t& files) {
  for (const auto& child : dir->children()) {
    if (IsDirectory(child.get())) {
      Count(child.get(), bytes, files);
    } else {
      bytes += child->size();
      ++files;
    }
  }
}

// Copies one file in 4 MB chunks so progress and cancel stay responsive.
bool CopyEntry(const DiscImageEntry* e, const fs::path& out_path, ExtractProgress& progress,
               std::string& error) {
  const uint8_t* src = e->mmap()->data() + e->data_offset();
  size_t size = e->data_size();
  if (e->data_offset() + size > e->mmap()->size()) {
    error = fmt::format("The disc image is truncated ({} is past its end). Re-dump the disc.",
                        e->name());
    return false;
  }
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = fmt::format("Can't create {}", bots::PathUtf8(out_path));
    return false;
  }
  constexpr size_t kChunk = 4 << 20;
  for (size_t pos = 0; pos < size; pos += kChunk) {
    if (progress.cancel.load()) return false;
    size_t n = std::min(kChunk, size - pos);
    out.write(reinterpret_cast<const char*>(src + pos), std::streamsize(n));
    if (!out) {
      error = fmt::format("Write failed for {} (disk full?)", bots::PathUtf8(out_path));
      return false;
    }
    progress.bytes_done += n;
  }
  return true;
}

bool ExtractDir(const Entry* dir, const fs::path& out_dir, ExtractProgress& progress,
                std::string& error) {
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) {
    error = fmt::format("Can't create folder {}: {}", bots::PathUtf8(out_dir), ec.message());
    return false;
  }
  for (const auto& child : dir->children()) {
    if (progress.cancel.load()) return false;
    fs::path out_path = out_dir / child->name();
    if (IsDirectory(child.get())) {
      if (!ExtractDir(child.get(), out_path, progress, error)) return false;
      continue;
    }
    progress.SetCurrent(child->path());
    if (!CopyEntry(static_cast<const DiscImageEntry*>(child.get()), out_path, progress, error)) {
      return false;
    }
    ++progress.files_done;
  }
  return true;
}

}  // namespace

uint32_t XexTitleId(const uint8_t* data, size_t size) {
  if (size < 0x18 || Be32(data) != 0x58455832) return 0;  // 'XEX2'
  uint32_t count = Be32(data + 0x14);
  for (uint32_t i = 0; i < count; ++i) {
    size_t at = 0x18 + size_t(i) * 8;
    if (at + 8 > size) return 0;
    if (Be32(data + at) == 0x00040006) {  // execution info: media id, version, base version, title id
      uint32_t info = Be32(data + at + 4);
      return size_t(info) + 0x10 <= size ? Be32(data + info + 0x0C) : 0;
    }
  }
  return 0;
}

ExtractResult ExtractIso(const fs::path& iso, const fs::path& dest, ExtractProgress& progress) {
  ExtractResult result;
  std::error_code ec;
#if defined(__ANDROID__)
  // A disc image picked on Android (src/android/android_app.cpp) is a descriptor the app holds, /proc/self/fd/<n>,
  // which stat may not be allowed to follow; the SDK maps the descriptor itself.
  if (!iso.native().starts_with("/proc/self/fd/") && !fs::is_regular_file(iso, ec)) {
#else
  if (!fs::is_regular_file(iso, ec)) {
#endif
    result.error = "That file doesn't exist.";
    return result;
  }
  rex::filesystem::DiscImageDevice device("\\SETUP", iso);
  if (!device.Initialize()) {
    result.error =
        "Not an Xbox 360 disc image (no XDVDFS filesystem found). Use a full ISO dump of the "
        "game disc.";
    return result;
  }
  auto* root = const_cast<Entry*>(device.root());
  auto* xex = root ? root->GetChild("default.xex") : nullptr;
  if (!xex || IsDirectory(xex)) {
    result.error = "This disc image has no default.xex, so it isn't a game disc.";
    return result;
  }
  auto* xex_entry = static_cast<DiscImageEntry*>(xex);
  uint32_t title = XexTitleId(xex_entry->mmap()->data() + xex_entry->data_offset(),
                              std::min<size_t>(xex_entry->data_size(), 0x10000));
  if (title != kTitleId) {
    result.error = fmt::format(
        "This is a different game (title id {:08X}). Use 50 Cent: Blood on the Sand ({:08X}).",
        title, kTitleId);
    return result;
  }

  uint64_t bytes = 0;
  uint32_t files = 0;
  Count(root, bytes, files);
  progress.bytes_total = bytes;
  progress.files_total = files;
  fs::path parent = dest.parent_path();
  fs::create_directories(parent, ec);
  auto space = fs::space(parent, ec);
  if (!ec && space.available < bytes + (256ull << 20)) {
    result.error = fmt::format("Not enough free space in {}: needs {:.1f} GB, {:.1f} GB free.",
                               bots::PathUtf8(parent), bytes / 1e9, space.available / 1e9);
    return result;
  }
  REXLOG_INFO("setup: extracting {} files ({:.2f} GB) from {} to {}", files, bytes / 1e9,
              bots::PathUtf8(iso), bots::PathUtf8(dest));

  fs::path partial = dest;
  partial += ".partial";
  fs::remove_all(partial, ec);
  if (!ExtractDir(root, partial, progress, result.error)) {
    fs::remove_all(partial, ec);
    if (progress.cancel.load()) result.error = "Cancelled.";
    REXLOG_WARN("setup: extraction stopped: {}", result.error);
    return result;
  }
  // Swap in the finished tree; an older install's files are replaced only now, and only those (B-181).
  if (!SwapInGameFiles(partial, dest, result.error)) {
    fs::remove_all(partial, ec);
    REXLOG_WARN("setup: install stopped: {}", result.error);
    return result;
  }
  REXLOG_INFO("setup: extraction done ({} files)", progress.files_done.load());
  result.ok = true;
  return result;
}

}  // namespace bots::setup
