// The launcher's look (launcher_menu.h): palette, type sizes and its fonts.
//
// Fonts: Inter (body) and Oswald (headings), both under the SIL Open Font License 1.1 (assets/fonts, OFL-*.txt), cut
// down to Latin, Greek and Cyrillic. The SDK's ImGui renderer uploads the font atlas once and can't add sizes later
// (no ImGuiBackendFlags_RendererHasTextures), so each text role is baked at the pixel size it is drawn at on this
// display (LauncherFonts::Build) and text is never a scaled-up bitmap. A window resized later draws from the nearest
// bake. Headings use only capitals, digits and punctuation, and the title only its own letters, which keeps the atlas
// small even at 4K.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <imgui.h>

namespace bots::setup {

// ---- Palette (sRGB, 0xAABBGGRR as ImGui packs it) ----
namespace color {
inline constexpr ImU32 Rgb(int r, int g, int b, int a = 255) {
  return ImU32(a) << 24 | ImU32(b) << 16 | ImU32(g) << 8 | ImU32(r);
}
inline constexpr ImU32 kBackground = Rgb(13, 11, 9);
inline constexpr ImU32 kPanel = Rgb(24, 20, 16, 235);
inline constexpr ImU32 kPanelBorder = Rgb(70, 58, 44);
inline constexpr ImU32 kRowFocus = Rgb(58, 44, 24, 235);
inline constexpr ImU32 kGold = Rgb(232, 169, 58);
inline constexpr ImU32 kGoldDim = Rgb(160, 116, 44);
inline constexpr ImU32 kText = Rgb(237, 227, 209);
inline constexpr ImU32 kTextDim = Rgb(168, 153, 138);
inline constexpr ImU32 kTextFaint = Rgb(120, 108, 96);
inline constexpr ImU32 kOnGold = Rgb(26, 18, 8);
inline constexpr ImU32 kGood = Rgb(122, 196, 104);
inline constexpr ImU32 kWarn = Rgb(242, 178, 72);
inline constexpr ImU32 kBad = Rgb(236, 106, 90);
inline constexpr ImU32 kTrack = Rgb(72, 62, 50);
inline constexpr ImU32 kShade = Rgb(0, 0, 0, 170);
}  // namespace color

// ---- Type: the size of each text role in layout pixels at scale 1 (a 600-pixel-high window) ----
enum class FontRole {
  kTitleBig,    // Oswald: "BLOOD ON THE SAND"
  kTitleSmall,  // Oswald: "50 CENT"
  kPageTitle,   // Oswald: a page's heading
  kMenu,        // Oswald: main menu entries
  kSubhead,     // Oswald: help title, group headings, card captions
  kLabel,       // Inter SemiBold: setting names
  kBody,        // Inter: help text, values, notes
  kSmall,       // Inter: folders, footnotes, version
  kPrompt,      // Inter SemiBold: button prompts and key caps
  kCount,
};

inline constexpr float kRoleSize[] = {46.0f, 22.0f, 34.0f, 25.0f, 17.0f, 16.0f, 14.5f, 12.5f, 13.0f};
static_assert(sizeof(kRoleSize) / sizeof(kRoleSize[0]) == size_t(FontRole::kCount));

// Oswald roles hold only these glyphs (headings are drawn in capitals).
inline bool IsHeadingRole(FontRole role) {
  return role == FontRole::kTitleBig || role == FontRole::kTitleSmall || role == FontRole::kPageTitle ||
         role == FontRole::kMenu || role == FontRole::kSubhead;
}

// The font files, as embedded in the game (CMake) or read from assets/fonts (the preview tool).
struct LauncherFontData {
  const void* inter_regular = nullptr;
  size_t inter_regular_size = 0;
  const void* inter_semibold = nullptr;
  size_t inter_semibold_size = 0;
  const void* oswald_semibold = nullptr;
  size_t oswald_semibold_size = 0;
  bool Valid() const { return inter_regular && inter_semibold && oswald_semibold; }
};

class LauncherFonts {
 public:
  // Adds the roles to `atlas`, baked for each of `pixel_scales` (screen pixels per layout pixel: the menu's scale times
  // the display scaling). Call before the atlas is built. Returns false without fonts (the menu then draws with the
  // atlas's default font).
  bool Build(ImFontAtlas* atlas, const LauncherFontData& data, std::vector<float> pixel_scales) {
    if (!data.Valid() || pixel_scales.empty()) return false;
    std::sort(pixel_scales.begin(), pixel_scales.end());
    std::vector<float> unique;
    for (float s : pixel_scales) {
      if (!(s > 0.0f)) continue;
      if (unique.empty() || s > unique.back() * 1.08f) unique.push_back(s);
    }
    for (float s : unique) {
      Set set;
      set.pixel_scale = s;
      for (int r = 0; r < int(FontRole::kCount); ++r) {
        const FontRole role = FontRole(r);
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;  // the data outlives the atlas (embedded, or kept by the caller)
        config.OversampleH = config.OversampleV = 1;
        config.PixelSnapH = true;
        const float px = std::round(kRoleSize[r] * s);
        const void* ttf = data.inter_regular;
        size_t size = data.inter_regular_size;
        const ImWchar* ranges = kRangesFull;
        if (role == FontRole::kTitleBig || role == FontRole::kTitleSmall) {
          ttf = data.oswald_semibold, size = data.oswald_semibold_size, ranges = kRangesTitle;
        } else if (IsHeadingRole(role)) {
          ttf = data.oswald_semibold, size = data.oswald_semibold_size, ranges = kRangesCaps;
        } else if (role == FontRole::kLabel || role == FontRole::kPrompt) {
          ttf = data.inter_semibold, size = data.inter_semibold_size, ranges = kRangesFull;
        }
        set.fonts[r] = atlas->AddFontFromMemoryTTF(const_cast<void*>(ttf), int(size), px, &config, ranges);
        set.baked_px[r] = px;
        if (!set.fonts[r]) return false;
      }
      sets_.push_back(set);
    }
    return !sets_.empty();
  }

  bool Ready() const { return !sets_.empty(); }

  // The font for `role` drawn at `pixel_scale`: the bake made for the nearest scale, preferring a larger one.
  ImFont* Font(FontRole role, float pixel_scale) const {
    if (sets_.empty()) return nullptr;
    const Set* best = &sets_.front();
    for (const auto& set : sets_) {
      if (set.pixel_scale >= pixel_scale * 0.97f) return set.fonts[int(role)];
      best = &set;
    }
    return best->fonts[int(role)];
  }

  // How far the nearest bake is from the size it is drawn at (1 = exact): for the log.
  float Mismatch(float pixel_scale) const {
    if (sets_.empty() || !(pixel_scale > 0.0f)) return 0.0f;
    for (const auto& set : sets_)
      if (set.pixel_scale >= pixel_scale * 0.97f) return set.pixel_scale / pixel_scale;
    return sets_.back().pixel_scale / pixel_scale;
  }

 private:
  struct Set {
    float pixel_scale = 1.0f;
    ImFont* fonts[int(FontRole::kCount)] = {};
    float baked_px[int(FontRole::kCount)] = {};
  };
  std::vector<Set> sets_;

  // Latin (with Latin-1 and Extended-A), Greek and Cyrillic, typographic punctuation: folder names in any of them.
  static constexpr ImWchar kRangesFull[] = {
      0x0020, 0x017F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2010, 0x2027, 0x2030, 0x203A, 0x20AC, 0x20AC, 0x2122, 0x2122,
      0x2190, 0x2193, 0xFFFD, 0xFFFD, 0,
  };
  // Headings: space to underscore (capitals, digits, punctuation).
  static constexpr ImWchar kRangesCaps[] = {0x0020, 0x005F, 0};
  // "50 CENT", "BLOOD ON THE SAND".
  static constexpr ImWchar kRangesTitle[] = {0x0020, 0x0020, 0x0030, 0x0030, 0x0035, 0x0035, 0x0041, 0x0045,
                                             0x0048, 0x0048, 0x004C, 0x004C, 0x004E, 0x004F, 0x0053, 0x0054, 0};
};

// The fonts embedded in the game (launcher_fonts.cpp; CMake writes the data from assets/fonts).
LauncherFontData EmbeddedLauncherFontData();

// The app's instance, baked into the overlay font atlas at startup (BotsApp::OnConfigureFonts).
inline LauncherFonts& LaunchFonts() {
  static LauncherFonts fonts;
  return fonts;
}

}  // namespace bots::setup
