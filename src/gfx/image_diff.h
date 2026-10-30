// Comparing the native renderer's output with the emulated frame (bots-replay and shadow mode).
#pragma once

#include "../path_utf8.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace bots::gfx {

// The presenter applies the guest's display gamma ramp (the 256-entry DC_LUT_30_COLOR table: red in bits 20-29,
// green 10-19, blue 0-9) to 8-bit front buffers, so the emulated reference is gamma-mapped. `rgb` is 8-bit RGB.
inline void ApplyGammaRamp(std::vector<uint8_t>& rgb, const uint32_t* table) {
  for (size_t i = 0; i < rgb.size(); ++i) {
    const uint32_t shift = (2 - i % 3) * 10;
    rgb[i] = uint8_t(((table[rgb[i]] >> shift) & 0x3FF) * 255 / 1023.0f + 0.5f);
  }
}

struct ImageDiff {
  double mean_abs = 0, psnr = 0;
  double bad_percent = 0;  // pixels with a channel off by more than 16
};

// Compares the overlapping area of two 8-bit RGB images (`ref_pixel_bytes` per reference pixel, red first).
inline ImageDiff CompareImages(const std::vector<uint8_t>& image, uint32_t w, uint32_t h, const uint8_t* reference,
                               uint32_t rw, uint32_t rh, size_t ref_stride, uint32_t ref_pixel_bytes) {
  const uint32_t cw = std::min(w, rw), ch = std::min(h, rh);
  double sum = 0, sq = 0;
  uint64_t bad = 0;
  for (uint32_t y = 0; y < ch; ++y) {
    const uint8_t* ref_row = reference + y * ref_stride;
    for (uint32_t x = 0; x < cw; ++x) {
      int worst = 0;
      for (int k = 0; k < 3; ++k) {
        const int d = std::abs(int(image[(size_t(y) * w + x) * 3 + k]) - int(ref_row[x * ref_pixel_bytes + k]));
        sum += d, sq += double(d) * d, worst = std::max(worst, d);
      }
      bad += worst > 16;
    }
  }
  ImageDiff diff;
  const double n = double(cw) * ch * 3;
  if (!n) return diff;
  const double mse = sq / n;
  diff.mean_abs = sum / n;
  diff.psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99.0;
  diff.bad_percent = 100.0 * double(bad) / (double(cw) * ch);
  return diff;
}

inline bool WritePpm(const std::filesystem::path& path, const uint8_t* data, uint32_t width, uint32_t height,
                     size_t stride, uint32_t pixel_bytes) {
  std::FILE* f = bots::OpenFile(path, "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", width, height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) std::fwrite(data + y * stride + x * pixel_bytes, 1, 3, f);
  }
  std::fclose(f);
  return true;
}

}  // namespace bots::gfx
