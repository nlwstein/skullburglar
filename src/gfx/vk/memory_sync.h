// Live guest memory for the native renderer (in the game only; bots-replay renders from a snapshot).
//
// The SDK's SharedMemory does the page bookkeeping: pages a draw requests are uploaded once and then
// write-protected through the guest memory's physical access callbacks; a CPU write drops the protection, marks
// the pages invalid (re-uploaded on their next request) and fires the watches. This subclass records the
// uploads into the renderer's command stream and queues the invalidated ranges for its texture caches.
//
// RequestRange takes the global critical region, which the game's threads take constantly; with ~1500 draws a frame
// the command processor spent a quarter of its time waiting for it. So Request first checks a lock-free copy of the
// valid-page bits and only goes to the SDK when a page in the range may be invalid.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>

#include "renderer.h"

namespace bots::gfx::vk {

class MemorySync final : public rex::graphics::SharedMemory, public GuestMemoryTracker {
 public:
  MemorySync(rex::memory::Memory& memory, Renderer& renderer);
  ~MemorySync() override;

  bool Request(uint32_t start, uint32_t length) override;
  void TakeInvalidated(std::vector<std::pair<uint32_t, uint32_t>>& ranges) override;

  // Diagnostics: requests, ones that took the lock and ones of those that uploaded (command-processor thread), and
  // invalidations: CPU writes to watched memory reported by the SDK (any thread; each one a write fault).
  uint64_t requests() const { return requests_; }
  uint64_t locked() const { return locked_; }
  uint64_t uploaded() const { return uploaded_; }
  uint64_t invalidations() const { return invalidations_.load(std::memory_order_relaxed); }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& page_ranges) override;

 private:
  // Any thread that wrote guest memory, inside the global critical region: only queues.
  static void OnInvalidated(const std::unique_lock<std::recursive_mutex>& global_lock, void* context,
                            uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu);

  Renderer& renderer_;
  GlobalWatchHandle watch_ = nullptr;
  std::mutex mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> invalidated_;
  // A page's bit is set before the SDK makes it valid and cleared whenever it is invalidated (OnInvalidated sees
  // every invalidation), so a set bit can be stale-cleared but never stale-set.
  std::unique_ptr<std::atomic<uint64_t>[]> valid_;
  void SetValidBits(uint32_t page_first, uint32_t page_last, bool valid);
  // Command-processor thread only: requests, ones that took the lock, and ones of those that uploaded.
  uint64_t requests_ = 0, locked_ = 0, uploaded_ = 0;
  std::atomic<uint64_t> invalidations_{0};
  bool uploading_ = false;
};

}  // namespace bots::gfx::vk
