#include "memory_sync.h"

namespace bots::gfx::vk {

MemorySync::MemorySync(rex::memory::Memory& memory, Renderer& renderer) : SharedMemory(memory), renderer_(renderer) {
  InitializeCommon();
  watch_ = RegisterGlobalWatch(OnInvalidated, this);
}

MemorySync::~MemorySync() {
  if (watch_) UnregisterGlobalWatch(watch_);
}

bool MemorySync::UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& page_ranges) {
  const uint32_t page_log2 = page_size_log2();
  for (const auto& [page_first, page_count] : page_ranges) {
    const uint32_t start = page_first << page_log2, length = page_count << page_log2;
    // Valid (and watched) before the copy: a write during it invalidates the pages again.
    MakeRangeValid(start, length, false);
    if (!renderer_.UploadGuestMemory(start, length)) return false;
  }
  return true;
}

void MemorySync::OnInvalidated(const std::unique_lock<std::recursive_mutex>&, void* context, uint32_t address_first,
                               uint32_t address_last, bool) {
  auto* self = static_cast<MemorySync*>(context);
  std::lock_guard lock(self->mutex_);
  if (!self->invalidated_.empty() && self->invalidated_.back().second + 1 == address_first) {
    self->invalidated_.back().second = address_last;
  } else {
    self->invalidated_.emplace_back(address_first, address_last);
  }
}

void MemorySync::TakeInvalidated(std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  std::lock_guard lock(mutex_);
  ranges.swap(invalidated_);
  invalidated_.clear();
}

}  // namespace bots::gfx::vk
