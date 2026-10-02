// On-disk records of a D3D frame capture (capture.cpp writes them, the replayer and tools/gfxcap.py read them).
#pragma once

#include <cstdint>

namespace bots::gfx {

#pragma pack(push, 1)
struct CallRecord {
  uint32_t entry, lr, thread, blob_count;
  uint32_t depth, pad;   // depth: hooked calls already open on this thread (nested runtime calls)
  uint64_t blob_offset;  // into blobs.bin: blob_count x {u32 guest address, u32 size, bytes}
  uint64_t r[8];         // r3..r10
  double f[8];           // f1..f8
  uint32_t stack[8];     // caller's r1+0x50.. (host order)
  uint64_t ret_r3;
  double ret_f1;
  uint64_t pm4_begin, pm4_end;  // byte offsets into pm4.bin
};
struct MemRegion {
  uint32_t physical;  // 1: address is physical (0..0x1FFFFFFF), 0: virtual
  uint32_t address, size;
  uint32_t pad;
  uint64_t offset;  // into mem.bin
};
#pragma pack(pop)

}  // namespace bots::gfx
