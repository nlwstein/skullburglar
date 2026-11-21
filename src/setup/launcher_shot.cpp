// See launcher_shot.h.
#include "launcher_shot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "ui_scale.h"

namespace bots::setup {

std::string ShotSize::Name() const {
  char text[48];
  if (std::fabs(dpi - 1.0f) < 0.01f) {
    std::snprintf(text, sizeof(text), "%dx%d", width, height);
  } else {
    std::snprintf(text, sizeof(text), "%dx%d@%g", width, height, double(dpi));
  }
  return text;
}

std::vector<ShotSize> ParseShotSizes(const std::string& text) {
  std::vector<ShotSize> out;
  std::stringstream in(text);
  std::string item;
  while (std::getline(in, item, ',')) {
    ShotSize s;
    float dpi = 1.0f;
    if (std::sscanf(item.c_str(), " %dx%d@%f", &s.width, &s.height, &dpi) >= 2 && s.width > 0 && s.height > 0 &&
        s.width <= 16384 && s.height <= 16384) {
      s.dpi = dpi > 0.0f ? dpi : 1.0f;
      out.push_back(s);
    }
  }
  return out;
}

std::vector<ScriptStep> ParseShotScript(const std::string& text) {
  std::string body = text;
  if (!body.empty() && body[0] == '@') {
    std::ifstream f(std::filesystem::path(body.substr(1)), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    body = ss.str();
  }
  // Comment lines go first (they may hold ';'), then lines and ';' both separate steps.
  std::string steps;
  std::stringstream lines(body);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t first = line.find_first_not_of(" \t\r");
    if (first != std::string::npos && line[first] == '#') continue;
    steps += line + ";";
  }
  for (char& c : steps)
    if (c == '\r') c = ';';
  std::vector<ScriptStep> out;
  std::stringstream in(steps);
  std::string item;
  while (std::getline(in, item, ';')) {
    item.erase(0, item.find_first_not_of(" \t"));
    item.erase(item.find_last_not_of(" \t") + 1);
    if (item.empty() || item[0] == '#') continue;
    const size_t colon = item.find(':');
    ScriptStep step;
    step.verb = item.substr(0, colon);
    if (colon != std::string::npos) step.arg = item.substr(colon + 1);
    out.push_back(step);
  }
  return out;
}

// ---- Rasterizer ----

namespace {

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

void Triangle(V a, V b, V c, const float clip[4], int width, int height, const Texture& tex, std::vector<uint8_t>& rgb) {
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

}  // namespace

void RasterizeDrawData(const ImDrawData* data, float scale, int width, int height, const ImTextureData* atlas,
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

// ---- PNG ----

namespace {

uint32_t Crc(const uint8_t* p, size_t n, uint32_t crc = 0) {
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

void Be32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

void Chunk(std::ofstream& f, const char* type, const std::vector<uint8_t>& data) {
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

}  // namespace

bool WritePng(const std::filesystem::path& path, int width, int height, const std::vector<uint8_t>& rgb) {
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

// ---- Scripts ----

namespace {

ImGuiKey KeyByName(std::string name) {
  static constexpr std::pair<const char*, const char*> kAlias[] = {
      {"Down", "DownArrow"}, {"Up", "UpArrow"},     {"Left", "LeftArrow"}, {"Right", "RightArrow"},
      {"Esc", "Escape"},     {"Shift", "LeftShift"}, {"Ctrl", "LeftCtrl"},  {"Del", "Delete"},
      {"Ins", "Insert"},     {"Return", "Enter"},    {"PgUp", "PageUp"},    {"PgDn", "PageDown"},
  };
  for (const auto& [a, b] : kAlias)
    if (name == a) name = b;
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
    const char* n = ImGui::GetKeyName(ImGuiKey(k));
    if (n && name == n) return ImGuiKey(k);
  }
  return ImGuiKey_None;
}

ImGuiKey PadByName(const std::string& name) {
  static constexpr std::pair<const char*, ImGuiKey> kPad[] = {
      {"A", ImGuiKey_GamepadFaceDown},   {"B", ImGuiKey_GamepadFaceRight}, {"X", ImGuiKey_GamepadFaceLeft},
      {"Y", ImGuiKey_GamepadFaceUp},     {"LB", ImGuiKey_GamepadL1},       {"RB", ImGuiKey_GamepadR1},
      {"LT", ImGuiKey_GamepadL2},        {"RT", ImGuiKey_GamepadR2},       {"Start", ImGuiKey_GamepadStart},
      {"Back", ImGuiKey_GamepadBack},    {"Up", ImGuiKey_GamepadDpadUp},   {"Down", ImGuiKey_GamepadDpadDown},
      {"Left", ImGuiKey_GamepadDpadLeft}, {"Right", ImGuiKey_GamepadDpadRight},
      {"StickUp", ImGuiKey_GamepadLStickUp}, {"StickDown", ImGuiKey_GamepadLStickDown},
  };
  for (const auto& [n, k] : kPad)
    if (name == n) return k;
  return ImGuiKey_None;
}

const char* ActionName(LauncherAction a) {
  switch (a) {
    case LauncherAction::kNone: return "none";
    case LauncherAction::kPlay: return "play";
    case LauncherAction::kQuit: return "quit";
    case LauncherAction::kBrowseIso: return "browse";
    case LauncherAction::kInstall: return "install";
    case LauncherAction::kCancelInstall: return "cancel-install";
    case LauncherAction::kShowReinstall: return "reinstall";
    case LauncherAction::kHideReinstall: return "keep";
    case LauncherAction::kChooseFolder: return "choose-folder";
    case LauncherAction::kUseDefaultFolder: return "default-folder";
    case LauncherAction::kUpdateNow: return "update";
    case LauncherAction::kCancelUpdate: return "cancel-update";
    case LauncherAction::kDownloadPage: return "download";
    case LauncherAction::kFixFirewall: return "firewall";
    case LauncherAction::kResetAll: return "reset";
    case LauncherAction::kMoveSaves: return "move-saves";
    case LauncherAction::kDismissSaves: return "dismiss-saves";
  }
  return "?";
}

bool PageByName(const std::string& name, LauncherPage* page) {
  static constexpr std::pair<const char*, LauncherPage> kNames[] = {
      {"main", LauncherPage::kMain},         {"graphics", LauncherPage::kGraphics}, {"hdr", LauncherPage::kHdr},
      {"game", LauncherPage::kGame},         {"controls", LauncherPage::kControls}, {"coop", LauncherPage::kCoop},
      {"files", LauncherPage::kGameFiles},   {"updates", LauncherPage::kUpdates},   {"about", LauncherPage::kAbout},
      {"bindings", LauncherPage::kBindings},
  };
  for (const auto& [n, p] : kNames)
    if (name == n) return *page = p, true;
  return false;
}

std::string PageName(LauncherPage page) {
  static constexpr const char* kNames[] = {"main", "graphics", "hdr", "game", "controls",
                                           "coop", "files",    "updates", "about", "bindings"};
  return kNames[int(page)];
}

}  // namespace

int RunShotScript(const ShotHost& host, const std::filesystem::path& dir, const std::vector<ShotSize>& sizes,
                  const std::vector<ScriptStep>& script) {
  int failures = 0;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream report(dir / "shots.txt", std::ios::app);
  auto log = [&](const std::string& line) {
    if (host.log) host.log(line);
    if (report) report << line << "\n";
  };
  ImGuiContext* previous = ImGui::GetCurrentContext();
  for (const ShotSize& size : sizes) {
    if (host.reset) host.reset();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    const float logical_w = float(size.width) / size.dpi, logical_h = float(size.height) / size.dpi;
    io.DisplaySize = ImVec2(logical_w, logical_h);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();
    LauncherFonts fonts;
    fonts.Build(io.Fonts, host.fonts, {LauncherMenuScale(logical_w, logical_h, std::getenv("BOTS_UI_SCALE")) * size.dpi});
    unsigned char* pixels = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th);
    io.Fonts->SetTexID(ImTextureID(1));
    LauncherMenu menu;
    PadKind pad = PadKind::kXbox;
    LauncherAction last_action = LauncherAction::kNone;
    auto frame = [&] {
      io.DeltaTime = 1.0f / 60.0f;
      ImGui::NewFrame();
      const LauncherAction a = host.frame(menu, fonts, size.dpi, pad);
      if (a != LauncherAction::kNone) last_action = a;
      ImGui::Render();
    };
    auto point = [&](const std::string& arg, size_t index) {
      float v[4] = {0, 0, 0, 0};
      std::sscanf(arg.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
      const float x = v[index * 2], y = v[index * 2 + 1];
      return ImVec2(x <= 1.0f ? x * logical_w : x, y <= 1.0f ? y * logical_h : y);
    };
    frame();
    frame();
    log("size " + size.Name() + ": atlas " + std::to_string(tw) + "x" + std::to_string(th));
    for (const ScriptStep& step : script) {
      const std::string& v = step.verb;
      if (v == "shot") {
        frame();
        frame();
        std::vector<uint8_t> rgb(size_t(size.width) * size_t(size.height) * 3, 0);
        RasterizeDrawData(ImGui::GetDrawData(), size.dpi, size.width, size.height, io.Fonts->TexData, rgb);
        const std::filesystem::path file = dir / (step.arg + "-" + size.Name() + ".png");
        const bool ok = WritePng(file, size.width, size.height, rgb);
        const LauncherLayoutReport& r = menu.report();
        char line[512];
        std::snprintf(line, sizeof(line),
                      "shot %s %s: page %s, rows %d, cut-off text %d, prompts left out %d, focus %s, smallest text "
                      "%.1f px%s",
                      step.arg.c_str(), size.Name().c_str(), PageName(menu.page()).c_str(), r.rows, r.truncated,
                      r.prompts_dropped, r.focus_visible ? "in view" : "OUT OF VIEW", double(r.min_text_px),
                      ok ? "" : " (NOT WRITTEN)");
        log(line);
        if (!ok) ++failures;
      } else if (v == "key" || v == "hold") {
        std::string name = step.arg;
        float seconds = 0.0f;
        if (v == "hold") {
          const size_t c = name.rfind(':');
          if (c != std::string::npos) seconds = std::strtof(name.c_str() + c + 1, nullptr), name = name.substr(0, c);
        }
        bool shift = false, ctrl = false;
        if (name.rfind("Shift+", 0) == 0) shift = true, name = name.substr(6);
        if (name.rfind("Ctrl+", 0) == 0) ctrl = true, name = name.substr(5);
        const ImGuiKey key = KeyByName(name);
        if (key == ImGuiKey_None) {
          log("script: unknown key " + name), ++failures;
          continue;
        }
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true), io.AddKeyEvent(ImGuiKey_LeftShift, true);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true), io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
        if (shift || ctrl) frame();  // the modifier is down first, as a player presses it
        io.AddKeyEvent(key, true);
        frame();
        for (int i = 0; i < int(seconds * 60.0f); ++i) frame();
        io.AddKeyEvent(key, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false), io.AddKeyEvent(ImGuiKey_LeftShift, false);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false), io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
        frame();
      } else if (v == "pad") {
        const ImGuiKey key = PadByName(step.arg);
        if (key == ImGuiKey_None) {
          log("script: unknown button " + step.arg), ++failures;
          continue;
        }
        io.AddKeyAnalogEvent(key, true, 1.0f);
        frame();
        io.AddKeyAnalogEvent(key, false, 0.0f);
        frame();
      } else if (v == "move") {
        const ImVec2 p = point(step.arg, 0);
        io.AddMousePosEvent(p.x, p.y);
        frame();
      } else if (v == "click" || v == "rclick") {
        const ImVec2 p = point(step.arg, 0);
        const int button = v == "click" ? 0 : 1;
        io.AddMousePosEvent(p.x, p.y);
        frame();
        io.AddMouseButtonEvent(button, true);
        frame();
        io.AddMouseButtonEvent(button, false);
        frame();
      } else if (v == "drag") {
        const ImVec2 a = point(step.arg, 0), b = point(step.arg, 1);
        io.AddMousePosEvent(a.x, a.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        for (int i = 1; i <= 8; ++i) {
          io.AddMousePosEvent(a.x + (b.x - a.x) * float(i) / 8.0f, a.y + (b.y - a.y) * float(i) / 8.0f);
          frame();
        }
        io.AddMouseButtonEvent(0, false);
        frame();
      } else if (v == "wheel") {
        io.AddMouseWheelEvent(0.0f, std::strtof(step.arg.c_str(), nullptr));
        frame();
        frame();
      } else if (v == "text") {
        io.AddInputCharactersUTF8(step.arg.c_str());
        frame();
      } else if (v == "page") {
        LauncherPage page;
        if (PageByName(step.arg, &page)) menu.GoTo(page);
        else log("script: unknown page " + step.arg), ++failures;
        frame();
      } else if (v == "focus") {
        menu.FocusSetting(step.arg);
        frame();
        frame();
      } else if (v == "row") {
        menu.FocusRow(std::atoi(step.arg.c_str()));
        frame();
      } else if (v == "device") {
        menu.SetLastInput(step.arg == "pad" ? LastInput::kController : LastInput::kKeyboardMouse);
        frame();
      } else if (v == "padkind") {
        pad = step.arg == "ps4" ? PadKind::kPlayStation4 : step.arg == "ps5" ? PadKind::kPlayStation5 : PadKind::kXbox;
        frame();
      } else if (v == "frames") {
        for (int i = 0; i < std::max(1, std::atoi(step.arg.c_str())); ++i) frame();
      } else if (v == "expect") {
        const size_t eq = step.arg.find('=');
        const std::string cvar = step.arg.substr(0, eq);
        const std::string want = eq == std::string::npos ? "" : step.arg.substr(eq + 1);
        const std::string got = host.value_of ? host.value_of(cvar) : "";
        const bool ok = ValuesEqual(got, want);
        log(std::string(ok ? "PASS" : "FAIL") + " " + size.Name() + " " + cvar + " = " + got + " (want " + want + ")");
        if (!ok) ++failures;
      } else if (v == "expect-page") {
        const bool ok = PageName(menu.page()) == step.arg;
        log(std::string(ok ? "PASS" : "FAIL") + " " + size.Name() + " page " + PageName(menu.page()) + " (want " +
            step.arg + ")");
        if (!ok) ++failures;
      } else if (v == "expect-action") {
        const bool ok = step.arg == ActionName(last_action);
        log(std::string(ok ? "PASS" : "FAIL") + " " + size.Name() + " action " + ActionName(last_action) + " (want " +
            step.arg + ")");
        if (!ok) ++failures;
        last_action = LauncherAction::kNone;
      } else if (v == "move-confirm") {
        menu.OpenMoveConfirm();
      } else if (v == "reset-confirm") {
        menu.OpenResetConfirm();
        frame();
      } else {
        log("script: unknown step " + v), ++failures;
      }
    }
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
  }
  return failures;
}

}  // namespace bots::setup
