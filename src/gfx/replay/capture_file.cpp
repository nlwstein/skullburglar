#include "capture_file.h"

#include <sys/mman.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace bots::gfx::replay {
namespace {

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path.string());
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

uint8_t* Reserve(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) throw std::runtime_error("mmap failed");
  return static_cast<uint8_t*>(p);
}

// meta.json is one flat object of integers.
uint64_t MetaField(const std::string& json, const std::string& key) {
  const auto at = json.find("\"" + key + "\"");
  if (at == std::string::npos) throw std::runtime_error("meta.json: no " + key);
  return std::stoull(json.substr(json.find(':', at) + 1));
}

}  // namespace

GuestMemory::GuestMemory() : physical_(Reserve(kPhysicalBytes)), virtual_(Reserve(kVirtualLimit)) {}

GuestMemory::~GuestMemory() {
  munmap(physical_, kPhysicalBytes);
  munmap(virtual_, kVirtualLimit);
}

uint8_t* GuestMemory::Virtual(uint32_t address) {
  return address >= kVirtualLimit ? Physical(VirtualToPhysical(address)) : virtual_ + address;
}

void Capture::Load(const std::filesystem::path& capture_dir) {
  dir = capture_dir;
  const auto meta_bytes = ReadAll(dir / "meta.json");
  const std::string meta(meta_bytes.begin(), meta_bytes.end());
  if (MetaField(meta, "version") != 2) throw std::runtime_error("unsupported capture version");
  if (MetaField(meta, "record_bytes") != sizeof(CallRecord)) throw std::runtime_error("call record size mismatch");
  frame = uint32_t(MetaField(meta, "frame"));
  device = uint32_t(MetaField(meta, "device"));
  front_buffer = uint32_t(MetaField(meta, "front_buffer"));

  const auto raw_calls = ReadAll(dir / "calls.bin");
  calls.resize(raw_calls.size() / sizeof(CallRecord));
  std::memcpy(calls.data(), raw_calls.data(), calls.size() * sizeof(CallRecord));
  blobs = ReadAll(dir / "blobs.bin");
  pm4 = ReadAll(dir / "pm4.bin");
  device_bytes = ReadAll(dir / "device.bin");

  const auto index = ReadAll(dir / "mem.idx");
  std::ifstream mem(dir / "mem.bin", std::ios::binary);
  if (!mem) throw std::runtime_error("cannot open mem.bin");
  for (size_t i = 0; i + sizeof(MemRegion) <= index.size(); i += sizeof(MemRegion)) {
    MemRegion r;
    std::memcpy(&r, index.data() + i, sizeof(r));
    uint8_t* dst = r.physical ? memory.Physical(r.address) : memory.Virtual(r.address);
    mem.seekg(std::streamoff(r.offset));
    mem.read(reinterpret_cast<char*>(dst), r.size);
    if (!mem) throw std::runtime_error("mem.bin truncated");
  }
}

int Capture::CallAt(uint64_t pm4_offset) const {
  int best = -1;
  for (size_t i = 0; i < calls.size(); ++i) {
    const CallRecord& c = calls[i];
    if (c.pm4_begin <= pm4_offset && pm4_offset < c.pm4_end &&
        (best < 0 || c.depth >= calls[size_t(best)].depth)) {
      best = int(i);
    }
  }
  return best;
}

}  // namespace bots::gfx::replay
