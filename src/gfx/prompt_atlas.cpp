// Keyboard/mouse (B-105) and PlayStation (B-160) button prompts; see prompt_atlas.h.
#include "prompt_atlas.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

#include <rex/cvar.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

// assets/prompts/{keyboard-mouse,playstation}.{png,xml}, embedded by CMake (prompt_glyphs.cpp in the build tree).
extern const unsigned char kPromptGlyphsPng[], kPlayStationGlyphsPng[];
extern const size_t kPromptGlyphsPngSize, kPlayStationGlyphsPngSize;
extern const unsigned char kPromptGlyphsXml[], kPlayStationGlyphsXml[];
extern const size_t kPromptGlyphsXmlSize, kPlayStationGlyphsXmlSize;

namespace bots::prompts {

namespace {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

struct Sheet {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  std::unordered_map<std::string, Rect> glyphs;
};

int Attribute(std::string_view tag, std::string_view name) {
  const size_t at = tag.find(std::string(" ") + std::string(name) + "=\"");
  return at == std::string_view::npos ? 0 : std::atoi(tag.data() + at + name.size() + 3);
}

Sheet LoadSheet(const unsigned char* png, size_t png_size, const unsigned char* xml_data, size_t xml_size) {
  Sheet s;
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(png, int(png_size), &s.width, &s.height, &channels, 4);
  if (!pixels) {
    std::fprintf(stderr, "prompts: glyph sheet failed to load: %s\n", stbi_failure_reason());
    return s;
  }
  s.rgba.assign(pixels, pixels + size_t(s.width) * s.height * 4);
  stbi_image_free(pixels);
  // <SubTexture name="keyboard_a" x="..." y="..." width="64" height="64" />
  const std::string_view xml(reinterpret_cast<const char*>(xml_data), xml_size);
  for (size_t at = xml.find("<SubTexture"); at != std::string_view::npos; at = xml.find("<SubTexture", at + 1)) {
    const std::string_view tag = xml.substr(at, xml.find('>', at) - at);
    const size_t name = tag.find("name=\"");
    if (name == std::string_view::npos) continue;
    const size_t end = tag.find('"', name + 6);
    // The sheet's y runs up from the bottom edge.
    const int h = Attribute(tag, "height");
    s.glyphs[std::string(tag.substr(name + 6, end - name - 6))] = {
        Attribute(tag, "x"), s.height - Attribute(tag, "y") - h, Attribute(tag, "width"), h};
  }
  return s;
}

const Sheet& KeyboardSheet() {
  static const Sheet sheet = LoadSheet(kPromptGlyphsPng, kPromptGlyphsPngSize, kPromptGlyphsXml, kPromptGlyphsXmlSize);
  return sheet;
}

const Sheet& PlayStationSheet() {
  static const Sheet sheet = LoadSheet(kPlayStationGlyphsPng, kPlayStationGlyphsPngSize, kPlayStationGlyphsXml,
                                       kPlayStationGlyphsXmlSize);
  return sheet;
}

// Key names as rex/ui/keybinds.cpp spells them (plus WheelUp / WheelDown) -> Kenney glyph names.
std::string GlyphName(std::string_view key) {
  static const std::unordered_map<std::string_view, const char*> kNames = {
      {"LMB", "mouse_left"}, {"RMB", "mouse_right"}, {"MMB", "mouse_scroll"},
      {"WheelUp", "mouse_scroll_up"}, {"WheelDown", "mouse_scroll_down"},
      {"Space", "keyboard_space"}, {"Return", "keyboard_enter"}, {"NumpadEnter", "keyboard_numpad_enter"},
      {"Escape", "keyboard_escape"}, {"Tab", "keyboard_tab"}, {"Backspace", "keyboard_backspace"},
      {"Shift", "keyboard_shift"}, {"Control", "keyboard_ctrl"}, {"Alt", "keyboard_alt"},
      {"Up", "keyboard_arrow_up"}, {"Down", "keyboard_arrow_down"}, {"Left", "keyboard_arrow_left"},
      {"Right", "keyboard_arrow_right"}, {"PageUp", "keyboard_page_up"}, {"PageDown", "keyboard_page_down"},
      {"CapsLock", "keyboard_capslock"}, {"NumLock", "keyboard_numlock"}, {"ScrollLock", "keyboard_scroll_lock"},
      {"PrintScreen", "keyboard_printscreen"}, {"Backtick", "keyboard_tilde"}, {"Plus", "keyboard_equals"},
      {"Slash", "keyboard_slash_forward"}, {"Backslash", "keyboard_slash_back"},
      {"LBracket", "keyboard_bracket_open"}, {"RBracket", "keyboard_bracket_close"}, {"Quote", "keyboard_apostrophe"},
      {"NumpadPlus", "keyboard_numpad_plus"}, {"NumpadStar", "keyboard_asterisk"},
  };
  if (const auto it = kNames.find(key); it != kNames.end()) return it->second;
  std::string name = "keyboard_";
  for (char c : key) name += char(std::tolower(static_cast<unsigned char>(c)));
  return name;  // letters, digits, F-keys, minus, comma, period, semicolon, delete, insert, home, end, pause
}

// The glyph of the first key in a comma-separated bind list that has one.
const Rect* GlyphFor(const Sheet& sheet, std::string_view keys) {
  while (!keys.empty()) {
    const size_t comma = keys.find(',');
    std::string_view key = keys.substr(0, comma);
    keys = comma == std::string_view::npos ? std::string_view() : keys.substr(comma + 1);
    while (!key.empty() && key.front() == ' ') key.remove_prefix(1);
    while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
    if (key.empty()) continue;
    if (const auto it = sheet.glyphs.find(GlyphName(key)); it != sheet.glyphs.end()) return &it->second;
  }
  return nullptr;
}

// Draws glyph scaled to a size x size square centred on (cx, cy): each output pixel averages 4x4 samples
// (premultiplied, so transparent edges don't darken).
void Draw(const Sheet& sheet, const Rect& glyph, float cx, float cy, int size, std::vector<uint8_t>& out) {
  const int x0 = int(cx - size * 0.5f + 0.5f), y0 = int(cy - size * 0.5f + 0.5f);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const int dx = x0 + x, dy = y0 + y;
      if (dx < 0 || dy < 0 || dx >= int(kButtonAtlasWidth) || dy >= int(kButtonAtlasHeight)) continue;
      float r = 0, g = 0, b = 0, a = 0;
      for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
          const int gx = glyph.x + std::min(glyph.w - 1, int((x + (sx + 0.5f) / 4) * glyph.w / size));
          const int gy = glyph.y + std::min(glyph.h - 1, int((y + (sy + 0.5f) / 4) * glyph.h / size));
          const uint8_t* p = &sheet.rgba[(size_t(gy) * sheet.width + gx) * 4];
          const float pa = p[3] / 255.0f;
          r += p[0] * pa, g += p[1] * pa, b += p[2] * pa, a += pa;
        }
      }
      uint8_t* q = &out[(size_t(dy) * kButtonAtlasWidth + dx) * 4];
      if (a > 0) {
        q[0] = uint8_t(std::min(255.0f, r / a + 0.5f));
        q[1] = uint8_t(std::min(255.0f, g / a + 0.5f));
        q[2] = uint8_t(std::min(255.0f, b / a + 0.5f));
      }
      q[3] = uint8_t(std::min(255.0f, a / 16 * 255 + 0.5f));
    }
  }
}

// Centre and glyph size of each button in the original atlas (measured from it; the game's UVs are 32x32 cells for
// A B X Y, 64x64 for the rest, 64x32 for Start and Back). Both sticks share one cell, drawn with an "L" or "R" over
// it: it shows the left-stick click (taunt).
struct Cell {
  float cx, cy;
  int size;
};
constexpr Cell kX{16, 16, 32}, kA{48, 16, 32}, kB{16, 48, 32}, kY{48, 48, 32}, kRB{96, 32, 40}, kLB{160, 32, 40},
    kStick{224, 32, 56}, kDpad{32, 96, 56}, kStart{96, 80, 30}, kBack{96, 112, 30}, kLT{160, 96, 44}, kRT{224, 96, 44};

std::vector<uint8_t> KeyboardAtlas() {
  const Sheet& sheet = KeyboardSheet();
  if (sheet.rgba.empty()) return {};
  std::vector<uint8_t> out(size_t(kButtonAtlasWidth) * kButtonAtlasHeight * 4, 0);
  static const struct {
    Cell cell;
    const char* bind;
  } kCells[] = {
      {kX, "kbm_x"},           {kA, "kbm_a"},
      {kB, "kbm_b"},           {kY, "kbm_y"},
      {kRB, "kbm_right_shoulder"}, {kLB, "kbm_left_shoulder"},
      {kStick, "kbm_left_thumb"},  {kDpad, "kbm_dpad_up"},
      {kStart, "kbm_start"},       {kBack, "kbm_back"},
      {kLT, "kbm_left_trigger"}, {kRT, "kbm_right_trigger"},
  };
  for (const auto& [cell, bind] : kCells) {
    const std::string keys = rex::cvar::Query<std::string>(bind);
    const Rect* glyph = nullptr;
    // Weapon select is four directions; the wheel says it best when it is bound.
    if (std::string_view(bind) == "kbm_dpad_up" && keys.find("Wheel") != std::string::npos) {
      if (const auto it = sheet.glyphs.find("mouse_scroll_vertical"); it != sheet.glyphs.end()) glyph = &it->second;
    }
    if (!glyph) glyph = GlyphFor(sheet, keys);
    if (glyph) Draw(sheet, *glyph, cell.cx, cell.cy, cell.size, out);
  }
  return out;
}

// By position, as SDL maps the pads: Cross is A, Circle B, Square X, Triangle Y; Options is Start; Share (DualShock 4)
// and Create (DualSense) are Back. The coloured face buttons read best at the size the HUD draws them.
std::vector<uint8_t> PlayStationAtlas(bool dualsense) {
  const Sheet& sheet = PlayStationSheet();
  if (sheet.rgba.empty()) return {};
  std::vector<uint8_t> out(size_t(kButtonAtlasWidth) * kButtonAtlasHeight * 4, 0);
  const struct {
    Cell cell;
    const char* glyph;
  } cells[] = {
      {kX, "playstation_button_color_square"},
      {kA, "playstation_button_color_cross"},
      {kB, "playstation_button_color_circle"},
      {kY, "playstation_button_color_triangle"},
      {kRB, "playstation_trigger_r1"},
      {kLB, "playstation_trigger_l1"},
      {kStick, "playstation_button_l3"},
      {kDpad, "playstation_dpad"},
      {kStart, dualsense ? "playstation5_button_options" : "playstation4_button_options"},
      {kBack, dualsense ? "playstation5_button_create" : "playstation4_button_share"},
      {kLT, "playstation_trigger_l2"},
      {kRT, "playstation_trigger_r2"},
  };
  for (const auto& [cell, name] : cells) {
    if (const auto it = sheet.glyphs.find(name); it != sheet.glyphs.end()) {
      Draw(sheet, it->second, cell.cx, cell.cy, cell.size, out);
    } else {
      std::fprintf(stderr, "prompts: no glyph %s\n", name);
    }
  }
  return out;
}

}  // namespace

std::vector<uint8_t> BuildButtonAtlas(PromptDevice device) {
  switch (device) {
    case PromptDevice::kKeyboardMouse:
      return KeyboardAtlas();
    case PromptDevice::kPlayStation4:
      return PlayStationAtlas(false);
    case PromptDevice::kPlayStation5:
      return PlayStationAtlas(true);
    default:
      return {};
  }
}

}  // namespace bots::prompts
