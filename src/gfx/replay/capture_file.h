// A D3D frame capture (src/gfx/capture.cpp) loaded for offline replay.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../capture_format.h"

namespace bots::gfx::replay {

// Guest memory as it was at the end of the captured frame. Pages the capture skipped (zero or uncommitted)
// read as zero.
class GuestMemory {
 public:
  GuestMemory();
  ~GuestMemory();
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  static constexpr uint32_t kPhysicalBytes = 0x20000000;
  static constexpr uint32_t kVirtualLimit = 0xA0000000;  // virtual heaps; above are the physical views

  // GPU physical address (0..0x1FFFFFFF).
  uint8_t* Physical(uint32_t address) { return physical_ + (address & (kPhysicalBytes - 1)); }
  // Guest virtual address; the 0xA0000000+ views map to physical memory.
  uint8_t* Virtual(uint32_t address);
  static uint32_t VirtualToPhysical(uint32_t address) {
    return (address & 0x1FFFFFFF) + (address >= 0xE0000000 ? 0x1000 : 0);
  }

 private:
  uint8_t* physical_ = nullptr;
  uint8_t* virtual_ = nullptr;
};

struct Capture {
  std::filesystem::path dir;
  uint32_t frame = 0, device = 0, front_buffer = 0;
  std::vector<CallRecord> calls;
  std::vector<uint8_t> blobs, pm4, device_bytes;
  GuestMemory memory;

  // Throws std::runtime_error.
  void Load(const std::filesystem::path& capture_dir);

  // The innermost call whose PM4 range contains this pm4.bin offset, or -1.
  int CallAt(uint64_t pm4_offset) const;
};

}  // namespace bots::gfx::replay
