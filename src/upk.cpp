#include "upk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bots::upk {

namespace {

constexpr uint32_t kPackageTag = 0x9E2A83C1;
constexpr uint32_t kCompressLzo = 2;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int Seek(std::FILE* f, uint64_t offset, int origin) {
#ifdef _WIN32
  return _fseeki64(f, int64_t(offset), origin);
#else
  return fseeko(f, off_t(offset), origin);
#endif
}

uint64_t Tell(std::FILE* f) {
#ifdef _WIN32
  return uint64_t(_ftelli64(f));
#else
  return uint64_t(ftello(f));
#endif
}

// Big-endian cursor over a buffer; `ok` turns false on any read past the end.
struct Cursor {
  const std::vector<uint8_t>& d;
  size_t at = 0;
  bool ok = true;
  uint32_t U32() {
    if (at + 4 > d.size()) {
      ok = false;
      return 0;
    }
    at += 4;
    return Be32(&d[at - 4]);
  }
  void Skip(size_t n) {
    at += n;
    ok = ok && at <= d.size();
  }
};

// The summary's compression flags and chunk table (tools/upk_decompress.py parse_header).
template <typename Chunk>
bool ParseSummary(const std::vector<uint8_t>& data, uint32_t& flags, std::vector<Chunk>& chunks) {
  Cursor r{data};
  if (r.U32() != kPackageTag) return false;
  r.Skip(8);  // version, total header size
  const int32_t folder = int32_t(r.U32());
  r.Skip(folder >= 0 ? size_t(folder) : size_t(-int64_t(folder)) * 2);
  r.Skip(4 + 7 * 4 + 16);  // package flags, name/export/import counts + offsets, depends offset, guid
  const int32_t generations = int32_t(r.U32());
  if (generations < 0 || generations > 4096) return false;
  r.Skip(size_t(generations) * 12 + 8);  // generations, engine build, cooker version
  flags = r.U32();
  const int32_t count = int32_t(r.U32());
  if (!r.ok || count < 0 || count > 1 << 16) return false;
  chunks.resize(size_t(count));
  for (Chunk& c : chunks) c = {r.U32(), r.U32(), r.U32(), r.U32()};
  return r.ok;
}

}  // namespace

bool LzoDecompress(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
  const uint8_t* ip = in;
  const uint8_t* const ip_end = in + in_size;
  uint8_t* op = out;
  uint8_t* const op_end = out + out_size;
  auto literals = [&](size_t n) {
    if (size_t(ip_end - ip) < n || size_t(op_end - op) < n) return false;
    std::memcpy(op, ip, n);
    ip += n, op += n;
    return true;
  };
  // Copies byte by byte: a match may overlap its own output (distance < length).
  auto match = [&](size_t distance, size_t n) {
    if (distance == 0 || distance > size_t(op - out) || size_t(op_end - op) < n) return false;
    const uint8_t* from = op - distance;
    while (n--) *op++ = *from++;
    return true;
  };
  // A run length continued in zero bytes (255 each) and a final byte.
  auto extend = [&](size_t base) -> size_t {
    size_t n = 0;
    while (ip < ip_end && *ip == 0) n += 255, ++ip;
    if (ip >= ip_end) return 0;
    return n + base + *ip++;
  };
  if (ip >= ip_end) return false;
  size_t t;
  // State after the previous instruction: 0 = a literal run may follow, 1-3 = that many literals were just copied,
  // 4 = a literal run of 4+ was just copied (the next short match then has the 0x800 bias).
  size_t state = 0;
  if (*ip > 17) {
    t = *ip++ - 17u;
    if (!literals(t)) return false;
    state = t < 4 ? t : 4;
  }
  for (;;) {
    if (ip >= ip_end) return false;
    t = *ip++;
    if (t < 16) {
      if (state == 0) {  // literal run
        if (t == 0 && !(t = extend(15))) return false;
        if (!literals(t + 3)) return false;
        state = 4;
        continue;
      }
      if (ip >= ip_end) return false;
      const size_t distance = (t >> 2) + (size_t(*ip++) << 2) + 1;
      if (state == 4) {  // 3-byte match right after a long literal run
        if (!match(distance + 0x800, 3)) return false;
      } else if (!match(distance, 2)) {
        return false;
      }
    } else if (t >= 64) {
      if (ip >= ip_end) return false;
      const size_t distance = ((t >> 2) & 7) + (size_t(*ip++) << 3) + 1;
      if (!match(distance, (t >> 5) + 1)) return false;
    } else if (t >= 32) {
      size_t length = t & 31;
      if (length == 0 && !(length = extend(31))) return false;
      if (ip_end - ip < 2) return false;
      const size_t distance = (ip[0] >> 2) + (size_t(ip[1]) << 6) + 1;
      ip += 2;
      if (!match(distance, length + 2)) return false;
    } else {  // 16..31
      size_t length = t & 7;
      if (length == 0 && !(length = extend(7))) return false;
      if (ip_end - ip < 2) return false;
      size_t distance = ((t & 8) << 11) + (ip[0] >> 2) + (size_t(ip[1]) << 6);
      ip += 2;
      if (distance == 0) return op == op_end && ip == ip_end;  // end of stream
      if (!match(distance + 0x4000, length + 2)) return false;
    }
    // The low two bits of the instruction's last byte: 0-3 literals follow.
    state = ip[-2] & 3;
    if (state && !literals(state)) return false;
  }
}

Package::Package(const std::filesystem::path& path) : file_(std::fopen(path.string().c_str(), "rb")) {
  if (!file_) return;
  Seek(file_, 0, SEEK_END);
  size_ = Tell(file_);
  std::vector<uint8_t> summary;
  uint32_t flags = 0;
  // Not a package, or an uncompressed one: the file reads as it is.
  if (!ReadRaw(0, size_t(std::min<uint64_t>(size_, 1 << 20)), summary) || !ParseSummary(summary, flags, chunks_) ||
      chunks_.empty()) {
    chunks_.clear();
    return;
  }
  compressed_ = true;
  valid_ = flags == kCompressLzo;
}

Package::~Package() {
  if (file_) std::fclose(file_);
}

bool Package::ReadRaw(uint64_t offset, size_t size, std::vector<uint8_t>& out) {
  out.resize(size);
  return offset + size <= size_ && Seek(file_, offset, SEEK_SET) == 0 && std::fread(out.data(), 1, size, file_) == size;
}

bool Package::Read(uint64_t offset, size_t size, std::vector<uint8_t>& out) {
  if (!file_ || !valid_) return false;
  if (!compressed_) return ReadRaw(offset, size, out);
  out.assign(size, 0);
  const uint64_t end = offset + size;
  uint64_t covered = 0;
  // The summary sits before the first chunk, verbatim.
  uint64_t first = UINT64_MAX;
  for (const Chunk& c : chunks_) first = std::min(first, c.uoff);
  if (offset < first) {
    const uint64_t n = std::min(end, first) - offset;
    std::vector<uint8_t> raw;
    if (!ReadRaw(offset, size_t(n), raw)) return false;
    std::memcpy(out.data(), raw.data(), size_t(n));
    covered += n;
  }
  std::vector<uint8_t> head, table, compressed, block;
  for (const Chunk& c : chunks_) {
    if (c.uoff >= end || c.uoff + c.usize <= offset) continue;
    // Chunk: tag, block size, compressed total, uncompressed total, then (compressed, uncompressed) per block.
    if (!ReadRaw(c.coff, 16, head) || Be32(&head[0]) != kPackageTag) return false;
    const uint64_t total = Be32(&head[12]);
    const uint64_t table_max = std::min<uint64_t>(size_ - std::min(size_, c.coff + 16), 1 << 16);
    if (total != c.usize || !ReadRaw(c.coff + 16, size_t(table_max), table)) return false;
    size_t blocks = 0;
    for (uint64_t sum = 0; sum < total; ++blocks) {
      if ((blocks + 1) * 8 > table.size()) return false;
      sum += Be32(&table[blocks * 8 + 4]);
    }
    uint64_t cpos = c.coff + 16 + blocks * 8, upos = c.uoff;
    for (size_t b = 0; b < blocks; ++b) {
      const uint32_t csize = Be32(&table[b * 8]), usize = Be32(&table[b * 8 + 4]);
      if (upos < end && upos + usize > offset) {
        block.resize(usize);
        if (!ReadRaw(cpos, csize, compressed) || !LzoDecompress(compressed.data(), csize, block.data(), usize)) {
          return false;
        }
        const uint64_t from = std::max(offset, upos), to = std::min(end, upos + usize);
        std::memcpy(out.data() + (from - offset), block.data() + (from - upos), size_t(to - from));
        covered += to - from;
      }
      cpos += csize, upos += usize;
    }
  }
  return covered == size;
}

}  // namespace bots::upk
