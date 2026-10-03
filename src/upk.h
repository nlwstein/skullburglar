// Reading byte ranges of the game's cooked packages (`*.xxx`: UE3, big-endian, LZO-compressed chunks) as if they were
// decompressed, the layout tools/upk_decompress.py writes: the summary verbatim, then every chunk at its uncompressed
// offset. Only the compressed blocks a range touches are read and decompressed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace bots::upk {

// LZO1X decompression of `in` into exactly `out_size` bytes; false on malformed input or a size mismatch.
bool LzoDecompress(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);

// A package (or any other file: one without compression chunks reads as it is) kept open for several reads.
class Package {
 public:
  explicit Package(const std::filesystem::path& path);
  ~Package();
  Package(const Package&) = delete;
  Package& operator=(const Package&) = delete;

  explicit operator bool() const { return file_ != nullptr; }
  // Bytes [offset, offset + size) of the decompressed package. False if the range isn't in it or the data is bad.
  bool Read(uint64_t offset, size_t size, std::vector<uint8_t>& out);

 private:
  struct Chunk {
    uint64_t uoff, usize, coff, csize;
  };
  bool ReadRaw(uint64_t offset, size_t size, std::vector<uint8_t>& out);

  std::FILE* file_ = nullptr;
  uint64_t size_ = 0;
  bool compressed_ = false, valid_ = true;
  std::vector<Chunk> chunks_;
};

}  // namespace bots::upk
