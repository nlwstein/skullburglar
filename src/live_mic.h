// Live scalar parameters of cooked material instances (the focus highlight on posters).
//
// The game changes a cooked material instance in place: while the player targets a scoring poster the engine writes the
// scalar parameter `Activated` = 1 into the poster's shared MaterialInstanceConstant, and every object drawn with that
// instance lights up (the material graph desaturates the diffuse and adds a pulsing lime glow). A cooked file only holds
// the unfocused value, so n1 has to be told. The scene feed watches the cooked instances of the meshes it feeds, remembers
// their parameters the first time it sees them, and names every instance whose live scalars differ as a material
// variant ("<path>?&s:Activated=1"); n1 draws every section that uses that instance with the variant. Pure parts, tested
// in tests/live_mic_test.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bots::livemic {

// BOTS_N1_FOCUS=0 turns the whole feature off. BOTS_N1_LIVE_PARAMS picks which parameters follow the game: "activated"
// (only the focus parameter) or "all" (any scalar that differs from its first-seen value).
enum class Mode : uint8_t { kOff, kActivated, kAll };

constexpr Mode ModeFrom(const char* focus, const char* params, Mode fallback) {
  if (focus && focus[0] == '0' && focus[1] == 0) return Mode::kOff;
  if (!params || !params[0]) return fallback;
  const std::string_view p(params);
  if (p == "0" || p == "off") return Mode::kOff;
  if (p == "all") return Mode::kAll;
  if (p == "activated") return Mode::kActivated;
  return fallback;
}

struct Param {
  std::string name;
  float value = 0;
};

inline bool SameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i];
    const char y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] + 32) : b[i];
    if (x != y) return false;
  }
  return true;
}

inline bool Watched(Mode mode, std::string_view name) {
  return mode == Mode::kAll || (mode == Mode::kActivated && SameName(name, "Activated"));
}

// "&s:Name=value" for every watched parameter of `live` that is missing from `first` or differs from it; "" when there is
// none (the instance looks as cooked). The engine adds a parameter to an instance's array the first time it sets it, so
// a name that `first` lacks counts as changed.
inline std::string Suffix(Mode mode, const std::vector<Param>& first, const std::vector<Param>& live) {
  std::string out;
  for (const Param& p : live) {
    if (!Watched(mode, p.name)) continue;
    bool same = false;
    for (const Param& q : first)
      if (SameName(p.name, q.name)) {
        same = std::fabs(p.value - q.value) <= 1e-5f * std::fmax(1.0f, std::fabs(q.value));
        break;
      }
    if (same) continue;
    char num[40];
    std::snprintf(num, sizeof(num), "%.6g", double(p.value));
    out += "&s:" + p.name + "=" + num;
  }
  return out;
}

// Hash step over the raw words of a parameter array (name index and value bits): equal hashes mean nothing changed.
constexpr uint64_t Mix(uint64_t h, uint32_t w) { return (h ^ w) * 1099511628211ull; }

// A cooked instance whose parameters change every frame (a fade the game drives) would make n1 build a variant per
// frame. Allows `kMaxChanges` changes per `kWindow` frames; the rest are ignored (the instance keeps its last variant).
struct ChurnGuard {
  static constexpr uint32_t kMaxChanges = 40;
  static constexpr uint64_t kWindow = 600;
  uint64_t window_start = 0;
  uint32_t changes = 0;
  bool Allow(uint64_t frame) {
    if (frame - window_start >= kWindow) window_start = frame, changes = 0;
    return ++changes <= kMaxChanges;
  }
};

}  // namespace bots::livemic
