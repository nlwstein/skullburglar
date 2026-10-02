#include "memory_sync.h"

namespace bots::gfx::vk {

MemorySync::MemorySync(rex::memory::Memory& memory, Renderer& renderer) : SharedMemory(memory), renderer_(renderer) {
  InitializeCommon();
  const size_t blocks = size_t(kBufferSize >> page_size_log2()) / 64;
  valid_ = std::make_unique<std::atomic<uint64_t>[]>(blocks);
  for (size_t i = 0; i < blocks; ++i) valid_[i].store(0, std::memory_order_relaxed);
  watch_ = RegisterGlobalWatch(OnInvalidated, this);
}

MemorySync::~MemorySync() {
  if (watch_) UnregisterGlobalWatch(watch_);
}

void MemorySync::SetValidBits(uint32_t page_first, uint32_t page_last, bool valid) {
  for (uint32_t block = page_first >> 6; block <= page_last >> 6; ++block) {
    const uint32_t lo = block == page_first >> 6 ? page_first & 63 : 0;
    const uint32_t hi = block == page_last >> 6 ? page_last & 63 : 63;
    const uint64_t bits = (hi == 63 ? ~uint64_t(0) : (uint64_t(1) << (hi + 1)) - 1) & ~((uint64_t(1) << lo) - 1);
    if (valid) {
      valid_[block].fetch_or(bits, std::memory_order_relaxed);
    } else {
      valid_[block].fetch_and(~bits, std::memory_order_relaxed);
    }
  }
}

bool MemorySync::Request(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize || kBufferSize - start < length) return RequestRange(start, length);
  const uint32_t page_first = start >> page_size_log2(), page_last = (start + length - 1) >> page_size_log2();
  bool all_valid = true;
  for (uint32_t block = page_first >> 6; all_valid && block <= page_last >> 6; ++block) {
    const uint32_t lo = block == page_first >> 6 ? page_first & 63 : 0;
    const uint32_t hi = block == page_last >> 6 ? page_last & 63 : 63;
    const uint64_t bits = (hi == 63 ? ~uint64_t(0) : (uint64_t(1) << (hi + 1)) - 1) & ~((uint64_t(1) << lo) - 1);
    all_valid = (valid_[block].load(std::memory_order_acquire) & bits) == bits;
  }
  if (all_valid) return true;
  // Set before the SDK validates the pages: an invalidation from here on clears the bits again.
  SetValidBits(page_first, page_last, true);
  if (RequestRange(start, length)) return true;
  SetValidBits(page_first, page_last, false);
  return false;
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
  self->SetValidBits(address_first >> self->page_size_log2(), address_last >> self->page_size_log2(), false);
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
