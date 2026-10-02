// Live guest memory for the native renderer (in the game only; bots-replay renders from a snapshot).
//
// The SDK's SharedMemory does the page bookkeeping: pages a draw requests are uploaded once and then
// write-protected through the guest memory's physical access callbacks; a CPU write drops the protection, marks
// the pages invalid (re-uploaded on their next request) and fires the watches. This subclass records the
// uploads into the renderer's command stream and queues the invalidated ranges for its texture caches.
#pragma once

#include <cstdint>
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

  bool Request(uint32_t start, uint32_t length) override { return RequestRange(start, length); }
  void TakeInvalidated(std::vector<std::pair<uint32_t, uint32_t>>& ranges) override;

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
};

}  // namespace bots::gfx::vk
