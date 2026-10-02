// BOTS_GFX_CAPTURE="<sec>[,<sec>...]": capture one frame of D3D calls per listed time (native renderer, P1).
//
// Seconds count from the first D3DDevice_Swap. The frame after that Swap is recorded until the next Swap, into
// $BOTS_GFX_CAPTURE_DIR/f<frame>/ (default dir: gfxcap):
//   calls.bin   every call into a D3D entry point (all threads): entry, return address, thread, r3-r10, f1-f8,
//               8 stack words from the caller's frame (args 9+ at +0x54), and the returned r3/f1
//   blobs.bin   guest bytes that are only valid at call time: shader constants, the viewport, UP vertex/index
//               data, Begin*Vertices data (copied at EndVertices)
//   pm4.bin     the PM4 the runtime wrote during the frame, in order; each call record has the [begin, end)
//               it emitted (nested calls nest). The runtime writes at the device's cursor (dev+0x30, pre-increment
//               stwu) and 0x8231FA50 starts a new segment when one fills; that function is hooked to stitch them
//   device.bin  the D3DDevice at frame start (0x4000 bytes)
//   mem.bin     committed guest memory at frame end (non-zero 4 KB pages; physical memory once, by physical
//               address), index in mem.idx
//   gamma.bin      the display gamma ramp (set by MMIO, so not in the PM4): 256 DC_LUT_30_COLOR entries, then
//                  128 x 3 DC_LUT_PWL_DATA entries (used for 2_10_10_10 front buffers)
//   reference.ppm  the emulated output of this frame (the render thread waits for the GPU before taking it)
//   meta.json
// Layouts: tools/gfxcap.py reads them. Guest data stays big-endian. Debug tool: it stalls the frame it records.
#include "capture.h"
#include "capture_format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>

namespace bots::gfx {
namespace {

constexpr uint32_t kDeviceGlobal = 0x838191B0;       // D3DDevice*
constexpr uint32_t kFrontBufferGlobal = 0x838191BC;  // texture UE3 resolves the frame into and passes to Swap
constexpr uint32_t kDeviceBytes = 0x4000;
constexpr uint32_t kMaxBlob = 8u << 20;
constexpr uint32_t kCursorOffset = 0x30;  // D3DDevice: PM4 write cursor (last written dword); limit at +0x38

// Entry points (knowledge/d3d-runtime.md).
constexpr uint32_t kSwap = 0x82317110;
constexpr uint32_t kNewSegment = 0x8231FA50;  // runtime-internal: submit the full segment, return the new cursor
constexpr uint32_t kSetVSConstantF = 0x823216B0;
constexpr uint32_t kSetPSConstantF = 0x82321790;
constexpr uint32_t kSetViewport = 0x82313448;
constexpr uint32_t kDrawVerticesUP = 0x82334A70;
constexpr uint32_t kDrawIndexedVerticesUP = 0x82334FE8;
constexpr uint32_t kBeginVertices = 0x823345C8;
constexpr uint32_t kBeginIndexedVertices = 0x82334AB8;
constexpr uint32_t kEndVertices = 0x82334A60;

enum class State { kIdle, kRecording };

struct Pending {  // Begin*Vertices data, copied when EndVertices runs
  uint32_t index_ptr = 0, index_size = 0, vertex_ptr = 0, vertex_size = 0;
};

struct Capture {
  std::mutex mutex;
  State state = State::kIdle;
  std::vector<double> times;  // trigger times left, ascending
  std::string dir;
  std::chrono::steady_clock::time_point first_swap{};
  bool seen_swap = false;
  uint64_t frame = 0;
  std::vector<CallRecord> calls;
  std::vector<uint8_t> blobs;
  std::vector<uint8_t> device;
  uint32_t device_ptr = 0;
  std::vector<uint8_t> pm4;
  uint32_t pm4_mark = 0;  // cursor value up to which pm4 holds the segment
  uint64_t pm4_lost = 0;  // cursor moved back without a hooked segment switch
};

Capture& Cap() {
  static Capture* c = [] {
    auto* c = new Capture;
    const char* list = std::getenv("BOTS_GFX_CAPTURE");
    for (const char* p = list ? list : ""; *p;) {
      char* end;
      double t = std::strtod(p, &end);
      if (end == p) break;
      c->times.push_back(t);
      p = *end ? end + 1 : end;
    }
    std::sort(c->times.begin(), c->times.end());
    const char* dir = std::getenv("BOTS_GFX_CAPTURE_DIR");
    c->dir = dir && *dir ? dir : "gfxcap";
    return c;
  }();
  return *c;
}

uint32_t ThreadIndex() {
  static std::atomic<uint32_t> next{0};
  thread_local uint32_t index = next++;
  return index;
}

thread_local Pending pending;
thread_local uint32_t call_depth = 0;  // recorded calls open on this thread

uint8_t* Guest(uint32_t address) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(address); }

uint32_t LoadBE32(uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Guest(address), 4);
  return __builtin_bswap32(v);
}

void AddBlob(Capture& c, CallRecord& rec, uint32_t address, uint64_t size) {
  if (!address || !size || size > kMaxBlob) return;
  const uint32_t header[2] = {address, uint32_t(size)};
  const uint8_t* h = reinterpret_cast<const uint8_t*>(header);
  c.blobs.insert(c.blobs.end(), h, h + sizeof(header));
  const uint8_t* src = Guest(address);
  c.blobs.insert(c.blobs.end(), src, src + size);
  ++rec.blob_count;
}

uint32_t IndexBytes(uint64_t format) { return format == 1 ? 2 : 4; }  // UE3 passes 1 (INDEX16) or 6 (INDEX32)

// Copies of data the call reads through a pointer argument that is only valid now.
void AddCallBlobs(Capture& c, CallRecord& rec) {
  const uint64_t* r = rec.r;  // r[0] = r3
  switch (rec.entry) {
    case kSetVSConstantF:
    case kSetPSConstantF:  // (dev, start, data, count float4s)
      AddBlob(c, rec, uint32_t(r[2]), r[3] * 16);
      break;
    case kSetViewport:  // (dev, D3DVIEWPORT9*)
      AddBlob(c, rec, uint32_t(r[1]), 24);
      break;
    case kDrawVerticesUP:  // (dev, type, vertexCount, data, stride)
      AddBlob(c, rec, uint32_t(r[3]), r[2] * r[4]);
      break;
    case kDrawIndexedVerticesUP:  // (dev, type, minIndex, numVertices, indexCount, indices, fmt, vertices; stride)
      AddBlob(c, rec, uint32_t(r[5]), r[4] * IndexBytes(r[6]));
      AddBlob(c, rec, uint32_t(r[7]), (r[2] + r[3]) * uint64_t(rec.stack[1]));
      break;
    case kEndVertices:
      AddBlob(c, rec, pending.index_ptr, pending.index_size);
      AddBlob(c, rec, pending.vertex_ptr, pending.vertex_size);
      pending = {};
      break;
    default:
      break;
  }
}

// Appends what the runtime wrote since the last mark: dwords (mark, cursor].
void FlushPm4(Capture& c) {
  const uint32_t cursor = LoadBE32(c.device_ptr + kCursorOffset);
  if (cursor > c.pm4_mark && cursor - c.pm4_mark < (16u << 20)) {
    const uint8_t* src = Guest(c.pm4_mark + 4);
    c.pm4.insert(c.pm4.end(), src, src + (cursor - c.pm4_mark));
  } else if (cursor != c.pm4_mark) {
    ++c.pm4_lost;
  }
  c.pm4_mark = cursor;
}

void WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
  std::ofstream out(path, std::ios::binary);
  out.write(static_cast<const char*>(data), std::streamsize(size));
}

// Committed, readable guest memory, skipping all-zero pages. Physical heaps (0xA0000000+) are views of one
// 512 MB physical memory: written once, by physical address.
void WriteMemory(const std::filesystem::path& dir, uint64_t* total) {
  auto* memory = rex::system::kernel_memory();
  std::ofstream data(dir / "mem.bin", std::ios::binary);
  std::vector<MemRegion> index;
  std::vector<bool> physical_done(0x20000000 / 4096);
  static const uint8_t zero[4096] = {};
  uint64_t offset = 0;
  auto emit = [&](bool physical, uint32_t address, const uint8_t* src, uint32_t size) {
    if (!index.empty()) {
      MemRegion& last = index.back();
      if (last.physical == uint32_t(physical) && last.address + last.size == address &&
          last.offset + last.size == offset) {
        last.size += size;
        data.write(reinterpret_cast<const char*>(src), size);
        offset += size;
        return;
      }
    }
    index.push_back({uint32_t(physical), address, size, 0, offset});
    data.write(reinterpret_cast<const char*>(src), size);
    offset += size;
  };
  for (uint64_t a = 0; a < 0x100000000ull;) {
    const uint32_t address = uint32_t(a);
    auto* heap = memory->LookupHeap(address);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(address, &info) || !info.region_size) {
      a = (a + 0x10000) & ~uint64_t(0xFFFF);
      continue;
    }
    const uint64_t end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, 0x100000000ull);
    if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead)) {
      const bool physical = heap->heap_type() == rex::memory::HeapType::kGuestPhysical;
      for (uint64_t p = std::max<uint64_t>(a, info.base_address); p < end; p += 4096) {
        const uint8_t* src = memory->TranslateVirtual<const uint8_t*>(uint32_t(p));
        if (std::memcmp(src, zero, 4096) == 0) continue;
        if (physical) {
          const uint32_t pa = memory->GetPhysicalAddress(uint32_t(p));
          if (pa == UINT32_MAX || physical_done[pa / 4096]) continue;
          physical_done[pa / 4096] = true;
          emit(true, pa, src, 4096);
        } else {
          emit(false, uint32_t(p), src, 4096);
        }
      }
    }
    a = std::max<uint64_t>(end, a + 4096);
  }
  WriteFile(dir / "mem.idx", index.data(), index.size() * sizeof(MemRegion));
  *total = offset;
}

// The command processor's gamma ramp getters are protected; a member pointer taken through a derived class reads
// them without patching the SDK.
struct GammaRamp : rex::graphics::CommandProcessor {
  static const void* Table(const CommandProcessor& cp) { return (cp.*&GammaRamp::gamma_ramp_256_entry_table)(); }
  static const void* Pwl(const CommandProcessor& cp) { return (cp.*&GammaRamp::gamma_ramp_pwl_rgb)(); }
};

void WriteGamma(const std::filesystem::path& path) {
  auto* gfx = static_cast<rex::graphics::GraphicsSystem*>(rex::system::kernel_state()->emulator()->graphics_system());
  auto* cp = gfx ? gfx->command_processor() : nullptr;
  if (!cp) return;
  std::vector<uint32_t> ramp(256 + 128 * 3);
  std::memcpy(ramp.data(), GammaRamp::Table(*cp), 256 * 4);
  std::memcpy(ramp.data() + 256, GammaRamp::Pwl(*cp), 128 * 3 * 4);
  WriteFile(path, ramp.data(), ramp.size() * 4);
}

void WriteReference(const std::filesystem::path& path) {
  auto* gfx = rex::system::kernel_state()->emulator()->graphics_system();
  auto* presenter = gfx ? gfx->presenter() : nullptr;
  rex::ui::RawImage img;
  if (!presenter || !presenter->CaptureGuestOutput(img) || !img.width) {
    REXLOG_WARN("gfx capture: no guest output for the reference image");
    return;
  }
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << img.width << " " << img.height << "\n255\n";
  for (uint32_t y = 0; y < img.height; ++y) {
    const uint8_t* row = img.data.data() + y * img.stride;
    for (uint32_t x = 0; x < img.width; ++x) out.write(reinterpret_cast<const char*>(row + x * 4), 3);
  }
}

// Render thread, at the Swap that ends the recorded frame (before the original runs).
void Finish(Capture& c, const std::filesystem::path& dir, uint64_t* mem_bytes) {
  std::filesystem::create_directories(dir);
  WriteFile(dir / "calls.bin", c.calls.data(), c.calls.size() * sizeof(CallRecord));
  WriteFile(dir / "blobs.bin", c.blobs.data(), c.blobs.size());
  WriteFile(dir / "device.bin", c.device.data(), c.device.size());
  WriteFile(dir / "pm4.bin", c.pm4.data(), c.pm4.size());
  WriteGamma(dir / "gamma.bin");
  WriteMemory(dir, mem_bytes);
}

void StartRecording(Capture& c) {
  c.state = State::kRecording;
  c.calls.clear();
  c.blobs.clear();
  c.pm4.clear();
  c.pm4_lost = 0;
  c.device_ptr = LoadBE32(kDeviceGlobal);
  c.pm4_mark = LoadBE32(c.device_ptr + kCursorOffset);
  const uint8_t* dev = Guest(c.device_ptr);
  c.device.assign(dev, dev + kDeviceBytes);
  REXLOG_INFO("gfx capture: recording frame {} (device {:08X})", c.frame, c.device_ptr);
}

}  // namespace

bool CaptureConfigured() {
  static const bool on = [] {
    const char* v = std::getenv("BOTS_GFX_CAPTURE");
    return v && *v;
  }();
  return on;
}

int64_t CapturePre(uint32_t entry, PPCContext& ctx) {
  Capture& c = Cap();
  std::unique_lock lock(c.mutex);
  if (c.state != State::kRecording) return -1;
  CallRecord rec{};
  rec.entry = entry;
  rec.lr = uint32_t(ctx.lr);
  rec.thread = ThreadIndex();
  rec.depth = call_depth;
  rec.blob_offset = c.blobs.size();
  const PPCRegister* gpr[8] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  const PPCRegister* fpr[8] = {&ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7, &ctx.f8};
  for (int i = 0; i < 8; ++i) {
    rec.r[i] = gpr[i]->u64;
    rec.f[i] = fpr[i]->f64;
    rec.stack[i] = LoadBE32(uint32_t(ctx.r1.u32 + 0x50 + 4 * i));
  }
  AddCallBlobs(c, rec);
  FlushPm4(c);
  rec.pm4_begin = rec.pm4_end = c.pm4.size();
  c.calls.push_back(rec);
  const int64_t token = int64_t(c.calls.size() - 1);
  if (entry != kSwap) {
    ++call_depth;
    return token;
  }

  // End of the recorded frame: the Swap belongs to it. Memory is taken before the original runs.
  const auto dir = std::filesystem::path(c.dir) / fmt::format("f{}", c.frame);
  uint64_t mem_bytes = 0;
  Finish(c, dir, &mem_bytes);
  REXLOG_INFO("gfx capture: frame {}: {} calls, {} blob bytes, {} KB PM4 ({} lost), {} MB memory -> {}", c.frame,
              c.calls.size(), c.blobs.size(), c.pm4.size() >> 10, c.pm4_lost, mem_bytes >> 20, dir.string());
  std::ofstream(dir / "meta.json") << fmt::format(
      "{{\"version\": 2, \"frame\": {}, \"device\": {}, \"device_bytes\": {}, \"front_buffer\": {}, "
      "\"calls\": {}, \"record_bytes\": {}, \"mem_bytes\": {}, \"pm4_bytes\": {}, \"pm4_lost\": {}}}\n",
      c.frame, c.device_ptr, kDeviceBytes, LoadBE32(kFrontBufferGlobal), c.calls.size(), sizeof(CallRecord),
      mem_bytes, c.pm4.size(), c.pm4_lost);
  c.state = State::kIdle;
  return -2;  // CapturePost takes the reference image once the original Swap has run
}

void CapturePost(uint32_t entry, int64_t token, PPCContext& ctx) {
  Capture& c = Cap();
  if (entry == kSwap) {
    std::unique_lock lock(c.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!c.seen_swap) c.seen_swap = true, c.first_swap = now;
    if (token == -2) {
      // Nothing else is submitted while the render thread waits here, so the GPU finishes this frame and the
      // presenter's guest output is exactly it.
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      WriteReference(std::filesystem::path(c.dir) / fmt::format("f{}", c.frame) / "reference.ppm");
      lock.lock();
    }
    ++c.frame;
    const double t = std::chrono::duration<double>(now - c.first_swap).count();
    if (c.state == State::kIdle && !c.times.empty() && t >= c.times.front()) {
      c.times.erase(c.times.begin());
      StartRecording(c);
    }
    return;
  }
  if (token < 0) return;
  --call_depth;
  std::unique_lock lock(c.mutex);
  if (size_t(token) >= c.calls.size()) return;
  if (entry == kNewSegment) {
    c.pm4_mark = LoadBE32(c.device_ptr + kCursorOffset);  // the old segment was flushed in CapturePre
  } else {
    FlushPm4(c);
  }
  CallRecord& rec = c.calls[size_t(token)];
  rec.pm4_end = c.pm4.size();
  rec.ret_r3 = ctx.r3.u64;
  rec.ret_f1 = ctx.f1.f64;
  const uint64_t* r = rec.r;
  if (entry == kBeginVertices && rec.ret_r3) {  // (dev, type, vertexCount, stride) -> data pointer
    pending = {0, 0, uint32_t(rec.ret_r3), uint32_t(r[2] * r[3])};
  } else if (entry == kBeginIndexedVertices && rec.ret_r3 == 0) {
    // (dev, type, baseVertex, numVertices, indexCount, fmt, stride, void** indices; void** vertices) -> HRESULT
    pending = {LoadBE32(uint32_t(r[7])), uint32_t(r[4] * IndexBytes(r[5])), LoadBE32(rec.stack[1]),
               uint32_t((r[2] + r[3]) * r[6])};
  }
}

}  // namespace bots::gfx
