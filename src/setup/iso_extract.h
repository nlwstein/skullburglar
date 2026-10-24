// Extract the game's files from the player's own Xbox 360 disc image (B-061).
// Reads the XDVDFS/GDFX filesystem with the SDK's DiscImageDevice, checks that default.xex is
// 50 Cent: Blood on the Sand (title id 5451083B), and copies every file into <dest>.partial,
// moved into <dest> only when complete, so an interrupted extraction never looks installed. A reinstall
// replaces only the disc's own entries in <dest> (game_swap.h); other files there are kept.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace bots::setup {

constexpr uint32_t kTitleId = 0x5451083B;

struct ExtractProgress {
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<uint64_t> bytes_total{0};
  std::atomic<uint32_t> files_done{0};
  std::atomic<uint32_t> files_total{0};
  std::atomic<bool> cancel{false};

  void SetCurrent(std::string name) {
    std::lock_guard lock(mutex_);
    current_ = std::move(name);
  }
  std::string Current() const {
    std::lock_guard lock(mutex_);
    return current_;
  }

 private:
  mutable std::mutex mutex_;
  std::string current_;
};

struct ExtractResult {
  bool ok = false;
  std::string error;  // user-facing, when !ok
};

// Blocking; run it on a worker thread. Safe to cancel through progress.cancel.
ExtractResult ExtractIso(const std::filesystem::path& iso, const std::filesystem::path& dest,
                         ExtractProgress& progress);

// Title id from a XEX2 header (execution info optional header), 0 if not found.
uint32_t XexTitleId(const uint8_t* data, size_t size);

}  // namespace bots::setup
