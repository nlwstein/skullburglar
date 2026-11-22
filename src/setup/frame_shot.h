// Screenshots of the launcher without a screen (test-only, B-400). No test machine can capture the launcher's window
// (it is drawn into the game's Vulkan surface, and macOS won't let a remote session record the screen), so with
// BOTS_SETUP_FRAME_SHOT=<dir> the launcher turns its own ImGui draw lists into pixels with a small software rasterizer
// at the end of a frame and writes a PNG. The rasterizer and PNG writer are the ones the full-screen launcher's
// BOTS_SETUP_SHOT hook uses (launcher_shot.cpp on the launcher-menu branch), copied here so this branch has no
// dependency on it. Dormant unless the variable is set.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

namespace bots::setup::frameshot {



struct Texture {
  const unsigned char* pixels = nullptr;
  int width = 0, height = 0, bpp = 4;
};

inline void Sample(const Texture& t, float u, float v, float out[4]) {
  if (!t.pixels || t.width <= 0) {
    out[0] = out[1] = out[2] = out[3] = 1.0f;
    return;
  }
  // Bilinear, clamped: text from a bake made for a nearby size stays smooth.
  const float x = u * float(t.width) - 0.5f, y = v * float(t.height) - 0.5f;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - float(x0), fy = y - float(y0);
  float acc[4] = {0, 0, 0, 0};
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const int sx = std::clamp(x0 + i, 0, t.width - 1), sy = std::clamp(y0 + j, 0, t.height - 1);
      const float w = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
      const unsigned char* p = t.pixels + (size_t(sy) * size_t(t.width) + size_t(sx)) * size_t(t.bpp);
      if (t.bpp == 1) {
        acc[0] += w, acc[1] += w, acc[2] += w, acc[3] += w * float(p[0]) / 255.0f;
      } else {
        for (int c = 0; c < 4; ++c) acc[c] += w * float(p[c]) / 255.0f;
      }
    }
  }
  for (int c = 0; c < 4; ++c) out[c] = acc[c];
}

struct V {
  float x, y, u, v, c[4];
};

inline float Edge(const V& a, const V& b, float px, float py) { return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x); }

inline void Triangle(V a, V b, V c, const float clip[4], int width, int height, const Texture& tex, std::vector<uint8_t>& rgb) {
  float area = Edge(a, b, c.x, c.y);
  if (std::fabs(area) < 1e-9f) return;
  if (area < 0) std::swap(b, c), area = -area;
  const int x0 = std::max({0, int(std::floor(std::min({a.x, b.x, c.x}))), int(std::floor(clip[0]))});
  const int y0 = std::max({0, int(std::floor(std::min({a.y, b.y, c.y}))), int(std::floor(clip[1]))});
  const int x1 = std::min({width - 1, int(std::ceil(std::max({a.x, b.x, c.x}))), int(std::ceil(clip[2])) - 1});
  const int y1 = std::min({height - 1, int(std::ceil(std::max({a.y, b.y, c.y}))), int(std::ceil(clip[3])) - 1});
  if (x0 > x1 || y0 > y1) return;
  // Top-left rule, so two triangles sharing an edge don't both cover its pixels (a seam in a translucent fill).
  auto top_left = [](const V& p, const V& q) { return (p.y == q.y && q.x < p.x) || q.y < p.y; };
  const float bias0 = top_left(b, c) ? 0.0f : 1e-6f;
  const float bias1 = top_left(c, a) ? 0.0f : 1e-6f;
  const float bias2 = top_left(a, b) ? 0.0f : 1e-6f;
  const float inv = 1.0f / area;
  // Text and most UI quads use one texel or one color; skip the sampling when the UVs don't vary.
  const bool flat_uv = a.u == b.u && b.u == c.u && a.v == b.v && b.v == c.v;
  float flat_tex[4];
  if (flat_uv) Sample(tex, a.u, a.v, flat_tex);
  for (int y = y0; y <= y1; ++y) {
    const float py = float(y) + 0.5f;
    if (py < clip[1] || py >= clip[3]) continue;
    for (int x = x0; x <= x1; ++x) {
      const float px = float(x) + 0.5f;
      if (px < clip[0] || px >= clip[2]) continue;
      const float w0 = Edge(b, c, px, py), w1 = Edge(c, a, px, py), w2 = Edge(a, b, px, py);
      if (w0 < bias0 || w1 < bias1 || w2 < bias2) continue;
      const float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
      float t[4];
      if (flat_uv) {
        std::memcpy(t, flat_tex, sizeof(t));
      } else {
        Sample(tex, a.u * l0 + b.u * l1 + c.u * l2, a.v * l0 + b.v * l1 + c.v * l2, t);
      }
      float s[4];
      for (int k = 0; k < 4; ++k) s[k] = (a.c[k] * l0 + b.c[k] * l1 + c.c[k] * l2) * t[k];
      const float alpha = std::clamp(s[3], 0.0f, 1.0f);
      if (alpha <= 0.0f) continue;
      uint8_t* d = &rgb[(size_t(y) * size_t(width) + size_t(x)) * 3];
      for (int k = 0; k < 3; ++k) {
        const float out = std::clamp(s[k], 0.0f, 1.0f) * 255.0f * alpha + float(d[k]) * (1.0f - alpha);
        d[k] = uint8_t(std::clamp(out + 0.5f, 0.0f, 255.0f));
      }
    }
  }
}


inline void RasterizeDrawData(const ImDrawData* data, float scale, int width, int height, const ImTextureData* atlas,
                       std::vector<uint8_t>& rgb) {
  if (!data) return;
  Texture tex;
  if (atlas && atlas->Pixels) {
    tex.pixels = static_cast<const unsigned char*>(atlas->Pixels);
    tex.width = atlas->Width;
    tex.height = atlas->Height;
    tex.bpp = atlas->BytesPerPixel;
  }
  const ImVec2 origin = data->DisplayPos;
  for (const ImDrawList* list : data->CmdLists) {
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback) continue;
      const float clip[4] = {(cmd.ClipRect.x - origin.x) * scale, (cmd.ClipRect.y - origin.y) * scale,
                             (cmd.ClipRect.z - origin.x) * scale, (cmd.ClipRect.w - origin.y) * scale};
      for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3) {
        V tri[3];
        for (int k = 0; k < 3; ++k) {
          const ImDrawVert& src = list->VtxBuffer[int(cmd.VtxOffset + list->IdxBuffer[int(cmd.IdxOffset + i + k)])];
          tri[k].x = (src.pos.x - origin.x) * scale;
          tri[k].y = (src.pos.y - origin.y) * scale;
          tri[k].u = src.uv.x;
          tri[k].v = src.uv.y;
          for (int c = 0; c < 4; ++c) tri[k].c[c] = float((src.col >> (8 * c)) & 0xFF) / 255.0f;
        }
        Triangle(tri[0], tri[1], tri[2], clip, width, height, tex, rgb);
      }
    }
  }
}


inline uint32_t Crc(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

inline void Be32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

inline void Chunk(std::ofstream& f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> head;
  Be32(head, uint32_t(data.size()));
  f.write(reinterpret_cast<const char*>(head.data()), 4);
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  f.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
  std::vector<uint8_t> crc;
  Be32(crc, Crc(body.data(), body.size()));
  f.write(reinterpret_cast<const char*>(crc.data()), 4);
}


inline bool WritePng(const std::filesystem::path& path, int width, int height, const std::vector<uint8_t>& rgb) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  static const uint8_t kSig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  f.write(reinterpret_cast<const char*>(kSig), sizeof(kSig));
  std::vector<uint8_t> ihdr;
  Be32(ihdr, uint32_t(width));
  Be32(ihdr, uint32_t(height));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  Chunk(f, "IHDR", ihdr);
  // Rows with filter byte 0, in stored (uncompressed) deflate blocks, with the zlib header and Adler-32.
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + std::ptrdiff_t(size_t(y) * size_t(width) * 3),
               rgb.begin() + std::ptrdiff_t(size_t(y + 1) * size_t(width) * 3));
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
  for (size_t at = 0; at < raw.size() || at == 0; at += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
    if (raw.empty()) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  Be32(z, (b << 16) | a);
  Chunk(f, "IDAT", z);
  Chunk(f, "IEND", {});
  return bool(f);
}





// The draw lists of every window in the current ImGui frame (after the last End()), as pixels, written to `path`.
// Returns false when there is nothing to draw or the file can't be written.
inline bool CaptureFrame(const std::filesystem::path& path) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g) return false;
  const ImGuiIO& io = ImGui::GetIO();
  const int width = int(io.DisplaySize.x * io.DisplayFramebufferScale.x);
  const int height = int(io.DisplaySize.y * io.DisplayFramebufferScale.y);
  if (width <= 0 || height <= 0) return false;
  ImDrawData data;
  data.Valid = true;
  data.DisplayPos = ImVec2(0, 0);
  data.DisplaySize = io.DisplaySize;
  for (ImGuiWindow* w : g->Windows) {
    if (w->Active && !w->Hidden && w->DrawList && w->DrawList->VtxBuffer.Size > 0) data.CmdLists.push_back(w->DrawList);
  }
  std::vector<uint8_t> rgb(size_t(width) * size_t(height) * 3, 24);  // the dark behind the launcher
  RasterizeDrawData(&data, io.DisplayFramebufferScale.x, width, height, io.Fonts->TexData, rgb);
  data.CmdLists.clear();  // the lists belong to the windows
  std::error_code ec;
  if (auto dir = path.parent_path(); !dir.empty()) std::filesystem::create_directories(dir, ec);
  return WritePng(path, width, height, rgb);
}

}  // namespace bots::setup::frameshot
