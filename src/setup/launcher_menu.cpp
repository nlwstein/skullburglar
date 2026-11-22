// See launcher_menu.h.
#include "launcher_menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ui_scale.h"

namespace bots::setup {

namespace {

// Button prompts: what each glyph stands for. kKey draws a key cap with the given text.
enum Btn { kA, kB, kX, kY, kLB, kRB, kLT, kRT, kStart, kDpadLR, kKey };

// Firewall states (firewall.h FirewallState), kept as numbers so the menu doesn't need that header.
enum Firewall { kFwNotApplicable, kFwChecking, kFwAllowed, kFwNotAsked, kFwBlocked, kFwPublic, kFwUnknown };

ImU32 Fade(ImU32 c, float alpha) {
  const int a = int(float((c >> 24) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (c & 0x00FFFFFFu) | (ImU32(a) << 24);
}

std::string Upper(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return s;
}

bool Contains(ImVec2 min, ImVec2 max, ImVec2 p) { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }

// Key names as the kbm_* cvars spell them (rex/ui/keybinds.cpp).
std::string KeyName(ImGuiKey key) {
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return std::string(1, char('A' + (key - ImGuiKey_A)));
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return std::string(1, char('0' + (key - ImGuiKey_0)));
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) return "F" + std::to_string(1 + (key - ImGuiKey_F1));
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) return "Numpad" + std::to_string(key - ImGuiKey_Keypad0);
  switch (key) {
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Enter: return "Return";
    case ImGuiKey_KeypadEnter: return "NumpadEnter";
    case ImGuiKey_Escape: return "Escape";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_LeftShift: case ImGuiKey_RightShift: return "Shift";
    case ImGuiKey_LeftCtrl: case ImGuiKey_RightCtrl: return "Control";
    case ImGuiKey_LeftAlt: case ImGuiKey_RightAlt: return "Alt";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_Delete: return "Delete";
    case ImGuiKey_Insert: return "Insert";
    case ImGuiKey_Home: return "Home";
    case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PageUp";
    case ImGuiKey_PageDown: return "PageDown";
    case ImGuiKey_CapsLock: return "CapsLock";
    case ImGuiKey_GraveAccent: return "Backtick";
    case ImGuiKey_Minus: return "Minus";
    case ImGuiKey_Equal: return "Plus";
    case ImGuiKey_Comma: return "Comma";
    case ImGuiKey_Period: return "Period";
    case ImGuiKey_Semicolon: return "Semicolon";
    case ImGuiKey_Slash: return "Slash";
    case ImGuiKey_Backslash: return "Backslash";
    case ImGuiKey_LeftBracket: return "LBracket";
    case ImGuiKey_RightBracket: return "RBracket";
    case ImGuiKey_Apostrophe: return "Quote";
    default: return {};
  }
}

bool Pressed(ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); }

// The main menu's entries: Play, the pages, Quit.
struct MainEntry {
  LauncherPage page;  // kMain: Play or Quit
  const char* label;
  const char* about;
};
constexpr int kMainPlay = 0;
// A portable-mode notice about the saves is showing (B-386 misplaced saves, B-389 misnamed portable.txt).
bool SavesNotice(const LauncherStatus& st) { return !st.saves_warning.empty() || !st.saves_note.empty(); }
std::vector<MainEntry> MainEntries() {
  std::vector<MainEntry> e;
  e.push_back({LauncherPage::kMain, "Play", "Start the game. Your settings are saved first."});
  for (const auto& p : kPages) e.push_back({p.page, p.title, p.about});
  e.push_back({LauncherPage::kMain, "Quit", "Close Skullburglar."});
  return e;
}

}  // namespace

// What the player asked for this frame, from the keyboard, a controller or the mouse. A row that uses an intent
// clears it, so one press does one thing.
struct LauncherMenu::Intent {
  bool up = false, down = false, left = false, right = false, big_left = false, big_right = false;
  bool accept = false, back = false, x = false, y = false, prev_page = false, next_page = false, play = false;
  bool mouse_moved = false, clicked = false;
  bool nav_moved = false;  // focus moved by a key or button this frame (scroll it into view)
};

struct LauncherMenu::RowState {
  int index = 0;
  ImVec2 min, max;
  bool focused = false, hovered = false, clicked = false, held = false;
};

LauncherMenu::LauncherMenu() {
  std::fill(std::begin(focus_), std::end(focus_), 0);
  main_focus_ = kMainPlay;
}

void LauncherMenu::GoTo(LauncherPage page) {
  page_ = page;
  scroll_to_focus_ = true;
  dragging_row_ = -1;
  if (page == LauncherPage::kMain) return;
  // The main menu keeps the entry that opened the page selected, so Back returns to it.
  const auto entries = MainEntries();
  for (int i = 0; i < int(entries.size()); ++i)
    if (entries[i].page == page) main_focus_ = i;
}

bool LauncherMenu::FocusSetting(const std::string& cvar) {
  focus_cvar_ = cvar;  // resolved while the page draws (its row index isn't known before)
  return true;
}

void LauncherMenu::FocusRow(int row) {
  if (page_ == LauncherPage::kMain) {
    main_focus_ = row;
  } else {
    focus_[int(page_)] = row;
  }
  scroll_to_focus_ = true;
}

ImFont* LauncherMenu::Font(FontRole role) const {
  ImFont* f = fonts_ && fonts_->Ready() ? fonts_->Font(role, pixel_scale_) : nullptr;
  return f ? f : ImGui::GetFont();
}

void LauncherMenu::CountText(FontRole role) {
  report_.min_text_px = std::min(report_.min_text_px, Size(role) * (pixel_scale_ / scale_));
}

ImVec2 LauncherMenu::Measure(FontRole role, const std::string& text, float wrap) const {
  return Font(role)->CalcTextSizeA(Size(role), FLT_MAX, wrap, text.c_str(), text.c_str() + text.size());
}

void LauncherMenu::Text(ImDrawList* dl, FontRole role, ImVec2 pos, ImU32 col, const std::string& text,
                        float max_width) {
  if (text.empty()) return;
  CountText(role);
  std::string shown = text;
  if (max_width > 0.0f && Measure(role, shown).x > max_width) {
    ++report_.truncated;
    const std::string dots = IsHeadingRole(role) ? "..." : "\xE2\x80\xA6";
    while (!shown.empty() && Measure(role, shown + dots).x > max_width) {
      shown.pop_back();
      while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) shown.pop_back();
      if (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0x80)) shown.pop_back();
    }
    shown += dots;
  }
  dl->AddText(Font(role), Size(role), pos, col, shown.c_str(), shown.c_str() + shown.size());
}

float LauncherMenu::TextWrapped(ImDrawList* dl, FontRole role, ImVec2 pos, ImU32 col, const std::string& text,
                                float width) {
  if (text.empty()) return 0.0f;
  CountText(role);
  const float w = std::max(width, S(20));
  dl->AddText(Font(role), Size(role), pos, col, text.c_str(), text.c_str() + text.size(), w);
  return Measure(role, text, w).y;
}

// ---- Glyphs: drawn from shapes, so they are sharp at every size and need no texture ----

float LauncherMenu::GlyphWidth(float h, int button, const char* key) const {
  if (button == kKey || device_ == PromptDevice::kKeyboardMouse) {
    const float tw = Font(FontRole::kPrompt)->CalcTextSizeA(h * 0.62f, FLT_MAX, 0.0f, key ? key : "").x;
    return std::max(h, tw + h * 0.6f);
  }
  switch (button) {
    case kLB: case kRB: case kLT: case kRT: return h * 1.45f;
    case kStart: return h * 1.3f;
    default: return h;
  }
}

float LauncherMenu::Glyph(ImDrawList* dl, ImVec2 p, float h, int button, const char* key) const {
  const float w = GlyphWidth(h, button, key);
  const ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
  const float r = h * 0.5f;
  const float line = std::max(1.0f, h * 0.08f);
  ImFont* font = Font(FontRole::kPrompt);
  auto centered = [&](const char* text, float size, ImU32 col) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, text);
  };
  if (button == kKey || device_ == PromptDevice::kKeyboardMouse) {
    // A key cap.
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), color::Rgb(46, 40, 34), h * 0.18f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), color::Rgb(150, 136, 118), h * 0.18f, 0, line);
    dl->AddLine(ImVec2(p.x + h * 0.15f, p.y + h - line * 1.5f), ImVec2(p.x + w - h * 0.15f, p.y + h - line * 1.5f),
                color::Rgb(90, 80, 70), line);
    centered(key ? key : "", h * 0.62f, color::kText);
    return w;
  }
  const bool ps = IsPlayStation(device_);
  switch (button) {
    case kA: case kB: case kX: case kY: {
      if (!ps) {
        static constexpr ImU32 kFill[] = {color::Rgb(74, 160, 64), color::Rgb(206, 64, 52), color::Rgb(52, 112, 214),
                                          color::Rgb(226, 176, 32)};
        static constexpr const char* kLetter[] = {"A", "B", "X", "Y"};
        dl->AddCircleFilled(c, r, kFill[button], 32);
        centered(kLetter[button], h * 0.66f, color::Rgb(250, 248, 240));
      } else {
        dl->AddCircleFilled(c, r, color::Rgb(40, 38, 40), 32);
        dl->AddCircle(c, r - line * 0.5f, color::Rgb(110, 104, 100), 32, line);
        const float s = r * 0.46f;
        if (button == kA) {  // cross
          const ImU32 col = color::Rgb(120, 160, 240);
          dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, line * 1.6f);
          dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), col, line * 1.6f);
        } else if (button == kB) {  // circle
          dl->AddCircle(c, s, color::Rgb(236, 96, 96), 24, line * 1.6f);
        } else if (button == kX) {  // square
          dl->AddRect(ImVec2(c.x - s * 0.85f, c.y - s * 0.85f), ImVec2(c.x + s * 0.85f, c.y + s * 0.85f),
                      color::Rgb(226, 130, 200), 0, 0, line * 1.6f);
        } else {  // triangle
          const ImU32 col = color::Rgb(96, 206, 170);
          dl->AddTriangle(ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y + s * 0.75f), ImVec2(c.x - s, c.y + s * 0.75f),
                          col, line * 1.6f);
        }
      }
      return w;
    }
    case kLB: case kRB: case kLT: case kRT: {
      static constexpr const char* kXbox[] = {"LB", "RB", "LT", "RT"};
      static constexpr const char* kPs[] = {"L1", "R1", "L2", "R2"};
      const int i = button - kLB;
      const float round = (button == kLT || button == kRT) ? h * 0.42f : h * 0.22f;
      dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), color::Rgb(40, 38, 40), round);
      dl->AddRect(p, ImVec2(p.x + w, p.y + h), color::Rgb(130, 122, 116), round, 0, line);
      centered(ps ? kPs[i] : kXbox[i], h * 0.56f, color::kText);
      return w;
    }
    case kStart: {
      dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), color::Rgb(40, 38, 40), h * 0.5f);
      dl->AddRect(p, ImVec2(p.x + w, p.y + h), color::Rgb(130, 122, 116), h * 0.5f, 0, line);
      for (int i = -1; i <= 1; ++i) {
        const float y = c.y + float(i) * h * 0.17f;
        dl->AddLine(ImVec2(c.x - h * 0.24f, y), ImVec2(c.x + h * 0.24f, y), color::kText, line * 1.2f);
      }
      return w;
    }
    case kDpadLR: {
      const float a = h * 0.18f, b = h * 0.48f;
      const ImU32 body = color::Rgb(70, 66, 64), lit = color::kText;
      dl->AddRectFilled(ImVec2(c.x - a, c.y - b), ImVec2(c.x + a, c.y + b), body, a * 0.4f);
      dl->AddRectFilled(ImVec2(c.x - b, c.y - a), ImVec2(c.x + b, c.y + a), body, a * 0.4f);
      dl->AddTriangleFilled(ImVec2(c.x - b + a * 0.4f, c.y), ImVec2(c.x - b + a * 1.6f, c.y - a * 0.7f),
                            ImVec2(c.x - b + a * 1.6f, c.y + a * 0.7f), lit);
      dl->AddTriangleFilled(ImVec2(c.x + b - a * 0.4f, c.y), ImVec2(c.x + b - a * 1.6f, c.y + a * 0.7f),
                            ImVec2(c.x + b - a * 1.6f, c.y - a * 0.7f), lit);
      return w;
    }
    default: return w;
  }
}

void LauncherMenu::Toggle(ImDrawList* dl, ImVec2 min, ImVec2 max, bool on, bool focused, bool enabled) {
  const float h = max.y - min.y;
  const float alpha = enabled ? 1.0f : 0.4f;
  dl->AddRectFilled(min, max, Fade(on ? color::kGold : color::kTrack, alpha), h * 0.5f);
  if (focused) dl->AddRect(min, max, Fade(color::kText, alpha), h * 0.5f, 0, std::max(1.0f, S(1.5f)));
  const float r = h * 0.5f - S(3);
  const ImVec2 knob(on ? max.x - h * 0.5f : min.x + h * 0.5f, (min.y + max.y) * 0.5f);
  dl->AddCircleFilled(knob, r, Fade(on ? color::kOnGold : color::kTextDim, alpha), 24);
}

// ---- Background: original shapes only (no game art) ----

void LauncherMenu::DrawBackground(ImDrawList* dl, const ImVec2& size, bool page) {
  const float w = size.x, h = size.y;
  dl->AddRectFilled(ImVec2(0, 0), size, color::kBackground);
  // A dusk sky: dark at the top, warm toward the horizon on the right.
  const float horizon = h * (page ? 0.98f : 0.72f);
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, horizon), color::Rgb(14, 11, 9), color::Rgb(34, 22, 14),
                              color::Rgb(120, 62, 24), color::Rgb(20, 14, 10));
  if (!page) {
    // The sun, low on the right, then the dunes in front of it.
    const ImVec2 sun(w * 0.80f, horizon - h * 0.10f);
    dl->AddCircleFilled(sun, h * 0.20f, color::Rgb(214, 150, 66, 60), 96);
    dl->AddCircleFilled(sun, h * 0.15f, color::Rgb(232, 172, 84, 150), 96);
    dl->AddRectFilledMultiColor(ImVec2(0, horizon - h * 0.08f), ImVec2(w, horizon), color::Rgb(0, 0, 0, 0),
                                color::Rgb(180, 96, 36, 0), color::Rgb(180, 96, 36, 110), color::Rgb(0, 0, 0, 0));
    auto dune = [&](float x0, float x1, float top, float base, ImU32 col) {
      dl->PathClear();
      dl->PathLineTo(ImVec2(x0, base));
      dl->PathBezierCubicCurveTo(ImVec2(x0 + (x1 - x0) * 0.3f, top), ImVec2(x0 + (x1 - x0) * 0.7f, top),
                                 ImVec2(x1, base), 48);
      dl->PathFillConvex(col);
    };
    dune(-w * 0.3f, w * 0.75f, horizon - h * 0.03f, h * 1.05f, color::Rgb(30, 19, 12));
    dune(w * 0.35f, w * 1.4f, horizon - h * 0.07f, h * 1.05f, color::Rgb(22, 15, 10));
    dl->AddRectFilled(ImVec2(0, horizon + h * 0.12f), ImVec2(w, h), color::Rgb(16, 11, 8));
    // Shade behind the menu column so its text keeps its contrast over the sky.
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w * 0.55f, h), color::Rgb(8, 6, 5, 200), color::Rgb(8, 6, 5, 0),
                                color::Rgb(8, 6, 5, 0), color::Rgb(8, 6, 5, 200));
  } else {
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h * 0.22f), color::Rgb(60, 34, 16, 140),
                                color::Rgb(110, 58, 22, 160), color::Rgb(0, 0, 0, 0), color::Rgb(0, 0, 0, 0));
  }
}

// ---- Frame ----

LauncherAction LauncherMenu::Draw(std::vector<Setting>& settings, const LauncherStatus& st,
                                  const LauncherFonts& fonts) {
  ImGuiIO& io = ImGui::GetIO();
  fonts_ = &fonts;
  action_ = LauncherAction::kNone;
  report_.Reset();
  sections_ = SectionsOf(settings);
  // An install that just finished (not cancelled, not failed): Back lands on Play, the obvious next step.
  if (was_extracting_ && !st.extracting && st.installed && st.install_error.empty()) main_focus_ = kMainPlay;
  was_extracting_ = st.extracting;
  // FocusSetting: open the setting's page; its row is selected while the page draws.
  if (!focus_cvar_.empty()) {
    if (std::none_of(settings.begin(), settings.end(), [&](const Setting& s) { return s.cvar == focus_cvar_; }))
      focus_cvar_.clear();  // not in this build
    for (size_t i = 0; i < settings.size(); ++i)
      if (settings[i].cvar == focus_cvar_ && PageOf(settings[i], sections_[i]) != page_)
        GoTo(PageOf(settings[i], sections_[i]));
  }
  // The menu does its own focus and navigation; ImGui's would move a second cursor with the same keys.
  io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);

  const ImVec2 display = io.DisplaySize;
  scale_ = LauncherMenuScale(display.x, display.y, std::getenv("BOTS_UI_SCALE"));
  pixel_scale_ = scale_ * std::max(st.dpi_scale, 0.25f);

  // Which device was used last: its glyphs go in the prompts.
  Intent in;
  const bool mouse_valid = io.MousePos.x > -1e8f && io.MousePos.y > -1e8f;
  in.mouse_moved = mouse_valid && last_mouse_.x > -1e8f &&
                   (std::fabs(io.MousePos.x - last_mouse_.x) > 0.5f || std::fabs(io.MousePos.y - last_mouse_.y) > 0.5f);
  if (mouse_valid) last_mouse_ = io.MousePos;
  in.clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  bool kbm_used = in.mouse_moved || in.clicked || io.MouseWheel != 0.0f;
  bool pad_used = false;
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
    if (!ImGui::IsKeyPressed(ImGuiKey(k), false)) continue;
    if (k >= ImGuiKey_GamepadStart && k <= ImGuiKey_GamepadRStickDown) {
      pad_used = true;
    } else if (k < ImGuiKey_GamepadStart) {
      kbm_used = true;
    }
  }
  if (!forced_input_) {
    if (pad_used) last_input_ = LastInput::kController;
    else if (kbm_used) last_input_ = LastInput::kKeyboardMouse;
  } else if (pad_used || kbm_used) {
    // A test forced a device; real input of the other kind takes over again.
    if (pad_used && last_input_ == LastInput::kKeyboardMouse) forced_input_ = false;
    if (kbm_used && !in.mouse_moved && last_input_ == LastInput::kController) forced_input_ = false;
  }
  std::string pad_prompts = "auto";
  for (const auto& s : settings)
    if (s.cvar == "pad_prompts") pad_prompts = s.value;
  device_ = LauncherPromptDevice(last_input_, st.pad, pad_prompts);

  // Keys and buttons. Not while a text box has the keyboard (the path field), a key binding waits for its key, or the
  // reset confirmation is open (those read their own input).
  text_input_active_ = io.WantTextInput;
  if (!text_input_active_ && capture_ < 0) {
    const bool shift = io.KeyShift;
    const bool ctrl = io.KeyCtrl || io.KeySuper;  // ImGui swaps Ctrl and Cmd on macOS: either works
    const bool tab = Pressed(ImGuiKey_Tab, true);
    in.up = Pressed(ImGuiKey_UpArrow, true) || Pressed(ImGuiKey_GamepadDpadUp, true) ||
            Pressed(ImGuiKey_GamepadLStickUp, true) || (tab && shift);
    in.down = Pressed(ImGuiKey_DownArrow, true) || Pressed(ImGuiKey_GamepadDpadDown, true) ||
              Pressed(ImGuiKey_GamepadLStickDown, true) || (tab && !shift);
    const bool left = Pressed(ImGuiKey_LeftArrow, true) || Pressed(ImGuiKey_GamepadDpadLeft, true) ||
                      Pressed(ImGuiKey_GamepadLStickLeft, true);
    const bool right = Pressed(ImGuiKey_RightArrow, true) || Pressed(ImGuiKey_GamepadDpadRight, true) ||
                       Pressed(ImGuiKey_GamepadLStickRight, true);
    in.left = left && !shift;
    in.right = right && !shift;
    in.big_left = (left && shift) || Pressed(ImGuiKey_GamepadL2, true);
    in.big_right = (right && shift) || Pressed(ImGuiKey_GamepadR2, true);
    const bool enter = Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter);
    in.accept = (enter && !ctrl) || Pressed(ImGuiKey_Space) || Pressed(ImGuiKey_GamepadFaceDown);
    in.back = Pressed(ImGuiKey_Escape) || Pressed(ImGuiKey_GamepadFaceRight);
    in.x = Pressed(ImGuiKey_Insert) || Pressed(ImGuiKey_GamepadFaceLeft);
    in.y = Pressed(ImGuiKey_Delete) || Pressed(ImGuiKey_GamepadFaceUp);
    in.prev_page = Pressed(ImGuiKey_Q) || Pressed(ImGuiKey_PageUp) || Pressed(ImGuiKey_GamepadL1);
    in.next_page = Pressed(ImGuiKey_E) || Pressed(ImGuiKey_PageDown) || Pressed(ImGuiKey_GamepadR1);
    in.play = Pressed(ImGuiKey_GamepadStart) || (enter && ctrl);
  }
  // A key binding waiting for its key or the reset confirmation takes the keys and buttons; nothing behind it moves.
  Intent modal_in;
  if (Busy()) {
    modal_in = in;
    in = Intent();
    in.mouse_moved = modal_in.mouse_moved;
  }

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(display);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(10));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, S(5));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, color::Rgb(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, color::kGoldDim);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, color::kGold);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, color::kGold);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
  if (ImGui::Begin("##bots_launcher", nullptr, flags)) {
    DrawBackground(ImGui::GetWindowDrawList(), display, page_ != LauncherPage::kMain);
    if (page_ == LauncherPage::kMain) {
      DrawMain(settings, st, in);
    } else {
      DrawPage(settings, st, in);
    }
  }
  ImGui::End();
  if (capture_ >= 0) DrawCapture(settings, modal_in);
  if (confirm_reset_ || confirm_move_) DrawConfirm(st, modal_in);
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(6);

  if (in.play && st.installed && !st.extracting && !st.update_busy) action_ = LauncherAction::kPlay;
  scroll_to_focus_ = false;
  return action_;
}

// ---- Main menu ----

void LauncherMenu::DrawMain(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const auto entries = MainEntries();
  const int n = int(entries.size());
  const bool can_play = st.installed && !st.extracting && !st.update_busy;
  // First run: start on Game files, where the install is.
  if (!started_) {
    started_ = true;
    if (!st.installed) {
      for (int i = 0; i < n; ++i)
        if (entries[i].page == LauncherPage::kGameFiles) main_focus_ = i;
    }
  }

  const float mx = std::max(S(28), size.x * 0.06f);
  const float footer_h = S(52);
  const float footer_top = size.y - footer_h;
  const float top = std::max(S(20), size.y * 0.07f);

  // Title.
  // Oswald's line box is tall for its capitals: the lines overlap their boxes so the title sits tight.
  Text(dl, FontRole::kTitleSmall, ImVec2(mx, top), color::kGold, "50 CENT");
  const float line1 = top + Size(FontRole::kTitleSmall) * 1.0f;
  Text(dl, FontRole::kTitleBig, ImVec2(mx, line1), color::kText, "BLOOD ON");
  Text(dl, FontRole::kTitleBig, ImVec2(mx, line1 + Size(FontRole::kTitleBig) * 0.92f), color::kText, "THE SAND");
  const float title_bottom = line1 + Size(FontRole::kTitleBig) * 2.05f;

  // Columns: the menu on the left, status cards and help on the right (under the menu when the window is narrow).
  const float col_w = std::clamp(size.x * 0.36f, S(260), S(520));
  const float right_x = mx + col_w + S(32);
  const float right_w = size.x - right_x - mx;
  const bool side = right_w >= S(240);
  const float list_top = title_bottom + S(18);
  float list_h = footer_top - list_top - S(12);
  if (!side) list_h -= S(70);  // the description goes under the menu
  // Entries share the height; below 30 layout pixels each the list scrolls instead.
  const float item_h = std::clamp(list_h / float(n), S(30), S(40));
  list_h = std::max(list_h, item_h * 3.0f);

  // Navigation.
  if (!Busy()) {
    if (in.up) main_focus_ = (main_focus_ + n - 1) % n, in.nav_moved = true;
    if (in.down) main_focus_ = (main_focus_ + 1) % n, in.nav_moved = true;
    if (in.back) main_focus_ = n - 1, in.nav_moved = true, in.back = false;  // B / Esc: to Quit, as consoles do
    if (in.prev_page) GoTo(LauncherPage::kAbout), in.prev_page = false;
    if (in.next_page) GoTo(LauncherPage::kGraphics), in.next_page = false;
    if ((in.y || Pressed(ImGuiKey_U)) && st.update_newer && st.update_can_install && !st.update_busy)
      action_ = LauncherAction::kUpdateNow;
  }
  if (in.nav_moved) scroll_to_focus_ = true;
  if (page_ != LauncherPage::kMain) return;

  ImGui::SetCursorScreenPos(ImVec2(mx - S(12), list_top));
  ImGui::BeginChild("##main_list", ImVec2(col_w + S(24), std::min(list_h, item_h * float(n))), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  ImDrawList* ldl = ImGui::GetWindowDrawList();
  row_index_ = 0;
  for (int i = 0; i < n; ++i) {
    const MainEntry& e = entries[i];
    const bool enabled = i != kMainPlay || can_play;
    ImGui::PushID(i);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##entry", ImVec2(col_w + S(24), item_h));
    const bool hovered = ImGui::IsItemHovered() && !Busy();
    if (hovered && in.mouse_moved) main_focus_ = i;
    const bool focused = main_focus_ == i;
    bool activate = hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (focused && in.accept && !Busy()) activate = true, in.accept = false;
    if (focused) {
      ldl->AddRectFilledMultiColor(p, ImVec2(p.x + col_w + S(24), p.y + item_h), Fade(color::kGold, enabled ? 1 : 0.4f),
                                   Fade(color::kGold, 0.0f), Fade(color::kGold, 0.0f),
                                   Fade(color::kGold, enabled ? 1 : 0.4f));
      ldl->AddRectFilled(p, ImVec2(p.x + col_w * 0.55f, p.y + item_h), Fade(color::kGold, enabled ? 1 : 0.4f));
      if (scroll_to_focus_) {
        const float wy = ImGui::GetWindowPos().y, wh = ImGui::GetWindowHeight();
        if (p.y < wy) ImGui::SetScrollY(ImGui::GetScrollY() - (wy - p.y));
        if (p.y + item_h > wy + wh) ImGui::SetScrollY(ImGui::GetScrollY() + (p.y + item_h - wy - wh));
      }
      const float wy = ImGui::GetWindowPos().y, wh = ImGui::GetWindowHeight();
      report_.focus_visible = p.y >= wy - 1 && p.y + item_h <= wy + wh + 1;
    }
    const ImU32 col = focused ? color::kOnGold : enabled ? color::kText : color::kTextFaint;
    Text(ldl, FontRole::kMenu, ImVec2(p.x + S(12), p.y + (item_h - Size(FontRole::kMenu) * 1.2f) * 0.5f), col,
         Upper(e.label), col_w);
    // A newer version shows on its entry too.
    if (e.page == LauncherPage::kUpdates && st.update_newer) {
      const float tw = Measure(FontRole::kMenu, Upper(e.label)).x;
      const ImVec2 b(p.x + S(12) + tw + S(10), p.y + item_h * 0.5f - S(9));
      const std::string badge = "NEW";
      const float bw = Measure(FontRole::kSubhead, badge).x + S(10);
      ldl->AddRectFilled(b, ImVec2(b.x + bw, b.y + S(18)), focused ? color::kOnGold : color::kGold, S(3));
      Text(ldl, FontRole::kSubhead, ImVec2(b.x + S(5), b.y), focused ? color::kGold : color::kOnGold, badge);
    }
    if (e.page == LauncherPage::kAbout && SavesNotice(st)) {
      const float tw = Measure(FontRole::kMenu, Upper(e.label)).x;
      const ImVec2 b(p.x + S(12) + tw + S(10), p.y + item_h * 0.5f - S(9));
      const float bw = Measure(FontRole::kSubhead, "!").x + S(14);
      ldl->AddRectFilled(b, ImVec2(b.x + bw, b.y + S(18)), focused ? color::kOnGold : color::kWarn, S(3));
      Text(ldl, FontRole::kSubhead, ImVec2(b.x + S(7), b.y), focused ? color::kGold : color::kOnGold, "!");
    }
    if (activate && enabled) {
      if (i == kMainPlay) action_ = LauncherAction::kPlay;
      else if (i == n - 1) action_ = LauncherAction::kQuit;
      else GoTo(e.page);
    }
    ImGui::PopID();
    ++row_index_;
  }
  report_.rows = n;
  ImGui::EndChild();

  // Status cards and the selected entry's description.
  const MainEntry& sel = entries[std::clamp(main_focus_, 0, n - 1)];
  std::string about = sel.about;
  if (main_focus_ == kMainPlay && !can_play) {
    about = st.extracting ? "The game files are being installed." : st.update_busy ? "An update is installing."
                                                                                     : "Install the game files first: open Game files.";
  }
  if (side) {
    const float cards_w = std::min(right_w, S(440));
    const float cards_x = size.x - mx - cards_w;
    DrawStatusCards(settings, st, ImVec2(cards_x, list_top), cards_w, footer_top - S(12));
    // The description sits under the cards.
    const float about_y = list_top + (SavesNotice(st) ? 4 : 3) * (S(60) + S(10));
    const float about_h = Measure(FontRole::kBody, about, cards_w - S(28)).y + S(24);
    if (about_y + about_h < footer_top) {
      dl->AddRectFilled(ImVec2(cards_x, about_y), ImVec2(cards_x + cards_w, about_y + about_h), color::kPanel, S(4));
      TextWrapped(dl, FontRole::kBody, ImVec2(cards_x + S(14), about_y + S(12)), color::kText, about, cards_w - S(28));
    } else {
      ++report_.truncated;
    }
  } else {
    const float y = list_top + list_h + S(8);
    TextWrapped(dl, FontRole::kBody, ImVec2(mx, y), color::kTextDim, about, size.x - 2 * mx);
  }
  DrawFooter(st, in, footer_top);
}

void LauncherMenu::DrawStatusCards(const std::vector<Setting>& settings, const LauncherStatus& st, ImVec2 pos,
                                   float width, float max_y) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto value = [&](const char* cvar) {
    for (const auto& s : settings)
      if (s.cvar == cvar) return s;
    return Setting{};
  };
  struct Card {
    const char* caption;
    std::string text;
    ImU32 dot;
    LauncherPage page;
  };
  std::vector<Card> cards;
  if (st.extracting) {
    char t[64];
    std::snprintf(t, sizeof(t), "Installing, %.0f%%", st.extract_fraction * 100.0f);
    cards.push_back({"GAME FILES", t, color::kGold, LauncherPage::kGameFiles});
  } else {
    cards.push_back({"GAME FILES", st.installed ? "Installed" : "Not installed: choose your disc image",
                     st.installed ? color::kGood : color::kWarn, LauncherPage::kGameFiles});
  }
  {
    Card c{"UPDATES", "", color::kTextFaint, LauncherPage::kUpdates};
    const Setting check = value("check_updates");
    if (!st.update_available_feature) c.text = "This build doesn't check for updates";
    else if (check.value == "false") c.text = "Checking is off";
    else if (st.update_busy) c.text = "Updating...", c.dot = color::kGold;
    else if (st.update_checking) c.text = "Checking...";
    else if (st.update_check_failed) c.text = "Couldn't check", c.dot = color::kWarn;
    else if (st.update_newer) c.text = st.update_latest + " is out", c.dot = color::kGold;
    else c.text = "Up to date", c.dot = color::kGood;
    cards.push_back(c);
  }
  {
    const Setting r = value("renderer"), hdr = value("vulkan_hdr"), scale = value("render_scale");
    std::string t = r.cvar.empty() ? "Default renderer" : r.value == "pre1" ? "Pre-1.0 renderer" : "New renderer";
    if (!hdr.cvar.empty()) t += hdr.value == "true" ? "  \xC2\xB7  HDR on" : "  \xC2\xB7  HDR off";
    if (!scale.cvar.empty()) t += "  \xC2\xB7  " + ValueLabel(scale, scale.value);
    cards.push_back({"RENDERER", t, color::kGood, LauncherPage::kGraphics});
  }
  if (SavesNotice(st)) {
    const bool can_move = st.saves_can_move && !st.saves_warning.empty();
    cards.push_back({"SAVES", !st.saves_warning.empty() ? (can_move ? "In the wrong folder: open About & logs to move them" : "In the wrong folder")
                              : st.saves_note_is_error ? "Needs a look: open About & logs" : "Moved: see About & logs",
                     st.saves_warning.empty() && !st.saves_note_is_error ? color::kGood : color::kWarn, LauncherPage::kAbout});
  }
  const float h = S(60);
  float y = pos.y;
  for (size_t i = 0; i < cards.size(); ++i) {
    const Card& c = cards[i];
    if (y + h > max_y) {
      ++report_.truncated;
      break;
    }
    const ImVec2 a(pos.x, y), b(pos.x + width, y + h);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(int(i));
    ImGui::InvisibleButton("##card", ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered() && !Busy();
    if (hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left)) GoTo(c.page);
    ImGui::PopID();
    dl->AddRectFilled(a, b, color::kPanel, S(4));
    dl->AddRect(a, b, hovered ? color::kGold : (c.dot == color::kGold ? color::kGoldDim : color::kPanelBorder), S(4), 0,
                std::max(1.0f, S(1)));
    Text(dl, FontRole::kSubhead, ImVec2(a.x + S(14), a.y + S(7)), color::kTextDim, c.caption);
    dl->AddCircleFilled(ImVec2(a.x + S(19), a.y + S(40)), S(4.5f), c.dot, 16);
    Text(dl, FontRole::kLabel, ImVec2(a.x + S(30), a.y + S(40) - Size(FontRole::kLabel) * 0.62f), color::kText, c.text,
         width - S(44));
    y += h + S(10);
  }
}

// ---- Settings pages ----

void LauncherMenu::DrawTabs(ImVec2 pos, float width, Intent& in) {
  (void)in;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float gh = S(22);
  const float gap = S(18);
  const LauncherPage current = page_ == LauncherPage::kBindings ? LauncherPage::kControls : page_;
  float total = 0.0f;
  for (const auto& p : kPages) total += Measure(FontRole::kLabel, p.tab).x + gap;
  const float lb_w = GlyphWidth(gh, kLB, "Q"), rb_w = GlyphWidth(gh, kRB, "E");
  const bool compact = total + lb_w + rb_w + S(16) > width;
  float x = pos.x;
  // LB / Q: previous section (clickable).
  auto glyph_button = [&](int button, const char* key, int dir) {
    const float w = GlyphWidth(gh, button, key);
    ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
    ImGui::PushID(dir);
    ImGui::InvisibleButton("##tabstep", ImVec2(w, gh));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !Busy()) GoTo(NeighborPage(page_, dir));
    ImGui::PopID();
    Glyph(dl, ImVec2(x, pos.y), gh, button, key);
    x += w + S(12);
  };
  glyph_button(kLB, "Q", -1);
  auto tab = [&](const PageInfo& p) {
    const ImVec2 ts = Measure(FontRole::kLabel, p.tab);
    const bool on = p.page == current;
    ImGui::SetCursorScreenPos(ImVec2(x, pos.y - S(2)));
    ImGui::PushID(p.tab);
    ImGui::InvisibleButton("##tab", ImVec2(ts.x, gh + S(6)));
    const bool hovered = ImGui::IsItemHovered() && !Busy();
    if (hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left)) GoTo(p.page);
    ImGui::PopID();
    Text(dl, FontRole::kLabel, ImVec2(x, pos.y + (gh - ts.y) * 0.5f), on ? color::kGold : hovered ? color::kText : color::kTextDim,
         p.tab);
    if (on) dl->AddRectFilled(ImVec2(x, pos.y + gh + S(3)), ImVec2(x + ts.x, pos.y + gh + S(5.5f)), color::kGold);
    x += ts.x + gap;
  };
  if (!compact) {
    for (const auto& p : kPages) tab(p);
  } else {
    // Not enough room for every section: the current one and its neighbors.
    const PageInfo* prev = FindPage(NeighborPage(current, -1));
    const PageInfo* cur = FindPage(current);
    const PageInfo* next = FindPage(NeighborPage(current, 1));
    for (const PageInfo* p : {prev, cur, next})
      if (p && x + Measure(FontRole::kLabel, p->tab).x + rb_w < pos.x + width) tab(*p);
  }
  glyph_button(kRB, "E", 1);
}

void LauncherMenu::DrawPage(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float mx = std::max(S(24), size.x * 0.045f);
  const float top = std::max(S(16), size.y * 0.035f);
  const float footer_h = S(52);
  const float footer_top = size.y - footer_h;

  // Page switching and back.
  if (!Busy()) {
    if (in.prev_page) GoTo(NeighborPage(page_, -1)), in.prev_page = false;
    if (in.next_page) GoTo(NeighborPage(page_, 1)), in.next_page = false;
    if (in.back && dragging_row_ < 0) {
      in.back = false;
      GoTo(page_ == LauncherPage::kBindings ? LauncherPage::kControls : LauncherPage::kMain);
      if (page_ == LauncherPage::kMain) {
        DrawMain(settings, st, in);
        return;
      }
    }
  }
  if (page_ == LauncherPage::kMain) {
    DrawMain(settings, st, in);
    return;
  }
  int& focus_now = focus_[int(page_)];
  if (!Busy() && prev_rows_ > 0) {
    if (in.up) focus_now = std::max(0, focus_now - 1), in.nav_moved = true;
    if (in.down) focus_now = std::min(prev_rows_ - 1, focus_now + 1), in.nav_moved = true;
  }
  if (in.nav_moved) scroll_to_focus_ = true;

  // Header: the page's name, then the section tabs.
  const PageInfo* info = FindPage(page_);
  const std::string title = page_ == LauncherPage::kBindings ? "KEY BINDINGS" : Upper(info ? info->title : "");
  Text(dl, FontRole::kPageTitle, ImVec2(mx, top), color::kText, title, size.x * 0.6f);
  const float tabs_y = top + Size(FontRole::kPageTitle) * 1.3f + S(4);
  DrawTabs(ImVec2(mx, tabs_y), size.x - 2 * mx, in);
  const float content_top = tabs_y + S(22) + S(18);
  dl->AddLine(ImVec2(mx, content_top - S(8)), ImVec2(size.x - mx, content_top - S(8)), color::kPanelBorder,
              std::max(1.0f, S(1)));

  // The list, and the help beside it (under it on a narrow or tall window).
  const float avail_w = size.x - 2 * mx;
  const float avail_h = footer_top - S(10) - content_top;
  const bool help_side = avail_w >= S(760);
  float list_w = avail_w, list_h = avail_h;
  ImVec2 help_min, help_max;
  if (help_side) {
    const float help_w = std::clamp(avail_w * 0.3f, S(240), S(420));
    list_w = avail_w - help_w - S(24);
    help_min = ImVec2(mx + list_w + S(24), content_top);
    help_max = ImVec2(size.x - mx, content_top + avail_h);
  } else {
    const float help_h = std::clamp(avail_h * 0.3f, S(84), S(150));
    list_h = avail_h - help_h - S(12);
    help_min = ImVec2(mx, content_top + list_h + S(12));
    help_max = ImVec2(size.x - mx, content_top + avail_h);
  }

  help_title_.clear(), help_text_.clear(), help_default_.clear(), help_note_.clear();
  help_locked_ = false;
  ImGui::SetCursorScreenPos(ImVec2(mx, content_top));
  ImGui::BeginChild("##page_list", ImVec2(list_w, std::max(list_h, S(60))), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
  row_index_ = 0;
  const float row_w = list_w - S(14);  // room for the scroll bar
  ImGui::PushID(int(page_));
  switch (page_) {
    case LauncherPage::kGameFiles: DrawGameFilesRows(st, in, row_w); break;
    case LauncherPage::kUpdates: DrawUpdatesRows(settings, st, in, row_w); break;
    case LauncherPage::kAbout: DrawAboutRows(settings, st, in, row_w); break;
    case LauncherPage::kBindings: DrawBindingRows(settings, in, row_w); break;
    default: DrawPageRows(settings, st, in, row_w); break;
  }
  ImGui::PopID();
  ImGui::Dummy(ImVec2(1, S(8)));
  prev_rows_ = row_index_;
  report_.rows = row_index_;
  if (row_index_ > 0) focus_now = std::clamp(focus_now, 0, row_index_ - 1);
  ImGui::EndChild();

  DrawHelp(help_min, help_max);
  DrawFooter(st, in, footer_top);
}

LauncherMenu::RowState LauncherMenu::BeginRow(const char* id, float height, float width) {
  RowState r;
  r.index = row_index_++;
  int& focus = focus_[int(page_)];
  r.min = ImGui::GetCursorScreenPos();
  r.max = ImVec2(r.min.x + width, r.min.y + height);
  ImGui::InvisibleButton(id, ImVec2(width, height));
  r.hovered = ImGui::IsItemHovered() && !Busy();
  r.clicked = r.hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left);
  r.held = ImGui::IsItemActive();
  if (r.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) focus = r.index;
  if (r.hovered && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x +
                           ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y > 0.25f &&
      dragging_row_ < 0)
    focus = r.index;
  // FocusSetting: once its row exists.
  if (focus_target_ >= 0 && focus_target_ == r.index) focus = r.index, focus_target_ = -1, scroll_to_focus_ = true;
  r.focused = focus == r.index;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (r.focused) {
    dl->AddRectFilled(r.min, r.max, color::kRowFocus, S(4));
    dl->AddRect(r.min, r.max, color::kGoldDim, S(4), 0, std::max(1.0f, S(1)));
    dl->AddRectFilled(r.min, ImVec2(r.min.x + S(4), r.max.y), color::kGold, S(2));
    const float wy = ImGui::GetWindowPos().y, wh = ImGui::GetWindowHeight();
    if (scroll_to_focus_) {
      // Into view with a little context; the first row scrolls to the top (headings above it), the last to the end
      // (notes under it).
      const float margin = S(24);
      if (r.index == 0) {
        ImGui::SetScrollY(0.0f);
      } else if (prev_rows_ > 0 && r.index == prev_rows_ - 1) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
      } else if (r.min.y - margin < wy) {
        ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - (wy - (r.min.y - margin))));
      } else if (r.max.y + margin > wy + wh) {
        ImGui::SetScrollY(ImGui::GetScrollY() + (r.max.y + margin - (wy + wh)));
      }
    }
    report_.focus_visible = r.min.y >= wy - 1 && r.max.y <= wy + wh + 1;
  } else if (r.hovered) {
    dl->AddRectFilled(r.min, r.max, color::Rgb(255, 255, 255, 10), S(4));
  }
  return r;
}

void LauncherMenu::EndRow(const RowState& row) { (void)row; }

void LauncherMenu::GroupHeading(const char* text, float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = S(40);
  Text(dl, FontRole::kSubhead, ImVec2(p.x + S(4), p.y + S(14)), color::kGold, Upper(text), width);
  dl->AddLine(ImVec2(p.x + S(4), p.y + h - S(3)), ImVec2(p.x + width, p.y + h - S(3)), color::kPanelBorder,
              std::max(1.0f, S(1)));
  ImGui::Dummy(ImVec2(width, h));
}

void LauncherMenu::Note(const std::string& text, ImU32 col, float width, FontRole role) {
  if (text.empty()) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = TextWrapped(dl, role, ImVec2(p.x + S(16), p.y + S(4)), col, text, width - S(28));
  ImGui::Dummy(ImVec2(width, h + S(10)));
}

void LauncherMenu::ProgressRow(float fraction, const std::string& overlay, float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = S(26);
  const ImVec2 a(p.x + S(16), p.y + S(4)), b(p.x + width - S(12), p.y + S(4) + h);
  dl->AddRectFilled(a, b, color::kTrack, S(4));
  dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * std::clamp(fraction, 0.0f, 1.0f), b.y), color::kGold, S(4));
  const ImVec2 ts = Measure(FontRole::kSmall, overlay);
  Text(dl, FontRole::kSmall, ImVec2((a.x + b.x - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f), color::kText, overlay);
  ImGui::Dummy(ImVec2(width, h + S(12)));
}

// One setting: its name on the left, its control on the right.
void LauncherMenu::SettingRow(Setting& s, const std::vector<Setting>& all, Intent& in, float width, bool disabled,
                              const char* disabled_note) {
  (void)all;
  const bool editable = !s.locked && !disabled;
  const float ctrl_w = std::clamp(width * 0.44f, S(180), S(380));
  const float label_w = width - ctrl_w - S(48);
  const float label_h = Measure(FontRole::kLabel, s.label, label_w).y;
  const float h = std::max(S(46), label_h + S(20));
  ImGui::PushID(s.cvar.c_str());
  RowState r = BeginRow("##row", h, width);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float alpha = editable ? 1.0f : 0.45f;
  TextWrapped(dl, FontRole::kLabel, ImVec2(r.min.x + S(18), r.min.y + (h - label_h) * 0.5f),
              Fade(color::kText, editable ? 1.0f : 0.6f), s.label, label_w);
  const ImVec2 cmin(r.max.x - ctrl_w - S(14), r.min.y + (h - S(30)) * 0.5f);
  const ImVec2 cmax(r.max.x - S(14), cmin.y + S(30));
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  bool changed = false;

  if (s.slider_max > 0) {
    double v = std::strtod(s.value.c_str(), nullptr);
    const float value_w = S(96);
    const ImVec2 tmin(cmin.x + S(8), (cmin.y + cmax.y) * 0.5f - S(3));
    const ImVec2 tmax(cmax.x - value_w - S(10), tmin.y + S(6));
    const float f = float(SliderFraction(s, v));
    dl->AddRectFilled(tmin, tmax, Fade(color::kTrack, alpha), S(3));
    dl->AddRectFilled(tmin, ImVec2(tmin.x + (tmax.x - tmin.x) * f, tmax.y), Fade(color::kGold, alpha), S(3));
    const ImVec2 knob(tmin.x + (tmax.x - tmin.x) * f, (tmin.y + tmax.y) * 0.5f);
    dl->AddCircleFilled(knob, S(r.focused ? 9 : 7.5f), Fade(color::kText, alpha), 24);
    if (r.focused) dl->AddCircle(knob, S(11), Fade(color::kGold, alpha), 24, std::max(1.0f, S(1.5f)));
    const std::string shown = DisplaySliderValue(s, v);
    const ImVec2 ts = Measure(FontRole::kBody, shown);
    Text(dl, FontRole::kBody, ImVec2(cmax.x - ts.x, (cmin.y + cmax.y - ts.y) * 0.5f), Fade(color::kText, alpha),
         shown, value_w);
    if (editable) {
      // The mouse: press on the track (or the knob) and drag.
      const ImVec2 hit_min(tmin.x - S(10), cmin.y - S(4)), hit_max(tmax.x + S(10), cmax.y + S(4));
      if (r.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && Contains(hit_min, hit_max, mouse))
        dragging_row_ = r.index;
      if (dragging_row_ == r.index) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          const double nf = (mouse.x - tmin.x) / std::max(1.0f, tmax.x - tmin.x);
          // Land on whole steps, as the keys do (a log slider keeps two decimals).
          const double nv = SnapSlider(s, SliderFromFraction(s, nf));
          if (FormatSliderValue(s, nv) != s.value) s.value = FormatSliderValue(s, nv), changed = true;
        } else {
          dragging_row_ = -1;
        }
      }
      if (r.focused) {
        int steps = 0;
        if (in.left) steps = -1, in.left = false;
        if (in.right) steps = 1, in.right = false;
        if (in.big_left) steps = -5, in.big_left = false;
        if (in.big_right) steps = 5, in.big_right = false;
        if (steps) {
          const std::string nv = FormatSliderValue(s, StepSlider(s, v, steps));
          if (nv != s.value) s.value = nv, changed = true;
        }
      }
    }
  } else if (s.choices.empty()) {
    const bool on = s.value == "true";
    const ImVec2 tmin(cmax.x - S(50), (cmin.y + cmax.y) * 0.5f - S(13));
    Toggle(dl, tmin, ImVec2(cmax.x, tmin.y + S(26)), on, r.focused, editable);
    const char* word = on ? "On" : "Off";
    const ImVec2 ts = Measure(FontRole::kBody, word);
    Text(dl, FontRole::kBody, ImVec2(tmin.x - S(12) - ts.x, (cmin.y + cmax.y - ts.y) * 0.5f),
         Fade(on ? color::kText : color::kTextDim, alpha), word);
    if (editable) {
      bool flip = r.clicked;
      if (r.focused && in.accept) flip = true, in.accept = false;
      if (r.focused && in.left && on) flip = true, in.left = false;
      if (r.focused && in.right && !on) flip = true, in.right = false;
      if (flip) s.value = on ? "false" : "true", changed = true;
    }
  } else {
    // A choice: < value >, with the arrows as buttons.
    dl->AddRectFilled(cmin, cmax, Fade(color::Rgb(20, 16, 13), alpha), S(4));
    dl->AddRect(cmin, cmax, Fade(r.focused ? color::kGold : color::kPanelBorder, alpha), S(4), 0,
                std::max(1.0f, S(1)));
    const float aw = S(28);
    const int at = ChoiceIndex(s);
    const bool can_left = at != 0, can_right = at != int(s.choices.size()) - 1;
    auto arrow = [&](float cx, int dir, bool lit) {
      const float a = S(5.5f), cy = (cmin.y + cmax.y) * 0.5f;
      const ImU32 col = Fade(lit ? color::kGold : color::kTextFaint, alpha);
      if (dir < 0)
        dl->AddTriangleFilled(ImVec2(cx - a * 0.6f, cy), ImVec2(cx + a * 0.6f, cy - a), ImVec2(cx + a * 0.6f, cy + a), col);
      else
        dl->AddTriangleFilled(ImVec2(cx + a * 0.6f, cy), ImVec2(cx - a * 0.6f, cy + a), ImVec2(cx - a * 0.6f, cy - a), col);
    };
    arrow(cmin.x + aw * 0.5f, -1, can_left);
    arrow(cmax.x - aw * 0.5f, 1, can_right);
    const std::string shown = ValueLabel(s, s.value);
    const float text_w = (cmax.x - cmin.x) - 2 * aw;
    FontRole role = FontRole::kBody;
    if (Measure(role, shown).x > text_w) role = FontRole::kSmall;
    const ImVec2 ts = Measure(role, shown);
    Text(dl, role, ImVec2(cmin.x + aw + std::max(0.0f, (text_w - ts.x) * 0.5f), (cmin.y + cmax.y - ts.y) * 0.5f),
         Fade(color::kText, alpha), shown, text_w);
    if (editable) {
      std::string nv = s.value;
      if (r.clicked && Contains(cmin, ImVec2(cmin.x + aw, cmax.y), mouse)) nv = StepChoice(s, -1, false);
      else if (r.clicked && Contains(ImVec2(cmax.x - aw, cmin.y), cmax, mouse)) nv = StepChoice(s, 1, false);
      else if (r.clicked && Contains(cmin, cmax, mouse)) nv = StepChoice(s, 1, true);
      if (r.focused) {
        if (in.left) nv = StepChoice(s, -1, false), in.left = false;
        if (in.right) nv = StepChoice(s, 1, false), in.right = false;
        if (in.accept) nv = StepChoice(s, 1, true), in.accept = false;
      }
      if (nv != s.value) s.value = nv, changed = true;
    }
  }
  (void)changed;
  if (r.focused && editable && in.y && !ValuesEqual(s.value, s.default_value)) {
    s.value = s.default_value;
    in.y = false;
  }
  if (r.focused) {
    help_title_ = s.label;
    help_text_ = s.help;
    // The old window's sliders took Ctrl-click to type a value (the F6 window still does); these step instead.
    for (const char* stale : {" Ctrl-click to type a value.", "Ctrl-click to type a value."})
      if (const size_t at = help_text_.find(stale); at != std::string::npos) help_text_.erase(at, std::strlen(stale));
    if (!s.default_value.empty()) help_default_ = "Default: " + ValueLabel(s, s.default_value);
    if (s.locked) {
      help_locked_ = true;
      help_note_ = "Set by a launch option (the command line or an environment variable), so it can't be changed "
                   "here.";
    } else if (disabled && disabled_note) {
      help_note_ = disabled_note;
    } else if (!s.choices.empty() && ChoiceIndex(s) < 0 && !s.value.empty()) {
      help_note_ = "Set to \"" + s.value + "\" in the settings file.";
    }
  }
  EndRow(r);
  ImGui::PopID();
}

bool LauncherMenu::ActionRow(const char* id, const std::string& label, const std::string& value,
                             const std::string& help, Intent& in, float width, bool enabled, bool primary) {
  const float value_w = value.empty() ? 0.0f : std::min(Measure(FontRole::kBody, value).x, width * 0.45f);
  const float label_w = width - value_w - S(60);
  const float label_h = Measure(FontRole::kLabel, label, label_w).y;
  const float h = std::max(S(46), label_h + S(20));
  RowState r = BeginRow(id, h, width);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (primary && enabled) {
    dl->AddRectFilled(ImVec2(r.min.x + S(8), r.min.y + S(6)), ImVec2(r.min.x + S(8) + Measure(FontRole::kLabel, label).x + S(24), r.max.y - S(6)),
                      r.focused ? color::kGold : color::kGoldDim, S(4));
    Text(dl, FontRole::kLabel, ImVec2(r.min.x + S(20), r.min.y + (h - Size(FontRole::kLabel) * 1.2f) * 0.5f),
         color::kOnGold, label, label_w);
  } else {
    TextWrapped(dl, FontRole::kLabel, ImVec2(r.min.x + S(18), r.min.y + (h - label_h) * 0.5f),
                enabled ? color::kText : color::kTextFaint, label, label_w);
  }
  if (!value.empty()) {
    Text(dl, FontRole::kBody, ImVec2(r.max.x - S(36) - value_w, r.min.y + (h - Size(FontRole::kBody) * 1.2f) * 0.5f),
         enabled ? color::kTextDim : color::kTextFaint, value, value_w);
  }
  // A chevron: this opens something.
  const float cx = r.max.x - S(20), cy = (r.min.y + r.max.y) * 0.5f, a = S(5);
  dl->AddTriangleFilled(ImVec2(cx + a * 0.6f, cy), ImVec2(cx - a * 0.6f, cy + a), ImVec2(cx - a * 0.6f, cy - a),
                        enabled ? (r.focused ? color::kGold : color::kTextDim) : color::kTextFaint);
  bool activate = enabled && r.clicked;
  if (enabled && r.focused && in.accept) activate = true, in.accept = false;
  if (r.focused) {
    help_title_ = label;
    help_text_ = help;
  }
  return activate;
}

void LauncherMenu::DrawPageRows(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in, float width) {
  auto value = [&](const char* cvar) {
    for (const auto& s : settings)
      if (s.cvar == cvar) return s.value;
    return std::string();
  };
  const bool kbm_off = value("kbm") == "false";
  std::string group;
  for (size_t i = 0; i < settings.size(); ++i) {
    Setting& s = settings[i];
    if (PageOf(s, sections_[i]) != page_) continue;
    if (page_ == LauncherPage::kControls) {
      const std::string g = sections_[i] == "Keyboard and mouse" ? "Keyboard and mouse" : "Controller";
      if (g != group) {
        if (!group.empty()) {
          // The key bindings open from the end of the controller group... no: they follow the keyboard group below.
        }
        GroupHeading(g.c_str(), width);
        group = g;
      }
    }
    if (!focus_cvar_.empty() && focus_cvar_ == s.cvar) focus_target_ = row_index_, focus_cvar_.clear();
    const bool kbm_dependent = s.cvar.starts_with("kbm_") && kbm_off;
    // Anti-aliasing and sharpening are new-renderer passes (greyed out with the pre-1.0 renderer, like HDR); the
    // pre-1.0 renderer's visibility test is the other way round.
    const bool pre1 = value("renderer") == "pre1";
    const bool n1_only = (s.cvar == "anti_aliasing" || s.cvar == "sharpening") && pre1;
    const bool pre1_only = s.cvar == "pre1_occlusion" && !pre1;
    const char* why = kbm_dependent ? "Turn on Keyboard and mouse to change this."
                      : n1_only     ? "Requires the new renderer."
                                    : "Only the pre-1.0 renderer uses this. Choose it as the renderer first.";
    SettingRow(s, settings, in, width, kbm_dependent || n1_only || pre1_only, why);
    // Notes that belong under a setting.
    if (s.cvar == "vulkan_hdr" && pre1) Note("HDR requires the new renderer.", color::kWarn, width);
    if (s.cvar == "sharpening" && pre1) Note("Anti-aliasing and sharpening require the new renderer.", color::kWarn, width);
    if (pre1_only) Note("Only the pre-1.0 renderer uses this setting.", color::kTextDim, width);
    if (s.cvar == "user_signin_live" && s.value == "true") {
      switch (st.firewall) {
        case kFwAllowed: Note("Windows Firewall: allowed.", color::kTextDim, width); break;
        case kFwNotAsked:
          Note("Windows is asking whether Skullburglar may use your network: choose Allow.", color::kWarn, width);
          break;
        case kFwPublic:
          Note("This network is set to Public, where Windows Firewall blocks LAN co-op. Set it to Private (Settings > "
               "Network & internet > your network > Private network).",
               color::kWarn, width);
          break;
        case kFwBlocked:
          Note("Windows Firewall is blocking LAN co-op: other players' games won't show up.", color::kWarn, width);
          if (ActionRow("##fw", st.firewall_fixing ? "Waiting for Windows..." : "Allow Skullburglar through the firewall",
                        "", st.firewall_fix_failed ? "Not changed. Windows asks for administrator permission."
                                                   : "Windows asks for administrator permission.",
                        in, width, !st.firewall_fixing))
            action_ = LauncherAction::kFixFirewall;
          break;
        default: break;
      }
    }
  }
  if (page_ == LauncherPage::kControls) {
    // The key bindings: a page of their own.
    bool any = false;
    for (const auto& s : settings) any |= s.binding;
    if (any) {
      int changed = 0;
      for (const auto& s : settings)
        if (s.binding && !ValuesEqual(s.value, s.default_value)) ++changed;
      char v[48];
      std::snprintf(v, sizeof(v), changed ? "%d changed" : "Defaults", changed);
      if (ActionRow("##bindings", "Key bindings", v,
                    "Which keys and mouse buttons do what with keyboard and mouse. Each action can have several keys.",
                    in, width, !kbm_off))
        GoTo(LauncherPage::kBindings);
      if (kbm_off) Note("Turn on Keyboard and mouse to change the key bindings.", color::kTextDim, width);
    }
  }
}

void LauncherMenu::IsoPathRow(const LauncherStatus& st, Intent& in, float width) {
  const float h = S(52);
  RowState r = BeginRow("##iso_row", h, width);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Text(dl, FontRole::kLabel, ImVec2(r.min.x + S(18), r.min.y + (h - Size(FontRole::kLabel) * 1.2f) * 0.5f),
       color::kText, "Disc image", S(150));
  if (r.focused && in.accept && !st.extracting) focus_text_input_ = true, in.accept = false;
  // The text box sits in the row (a mouse click goes straight to it).
  const float box_x = r.min.x + S(170);
  ImGui::SetCursorScreenPos(ImVec2(box_x, r.min.y + (h - Size(FontRole::kBody) * 1.2f - S(12)) * 0.5f));
  ImGui::PushFont(Font(FontRole::kBody), Size(FontRole::kBody));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(6)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(4));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, S(1)));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, color::Rgb(20, 16, 13));
  ImGui::PushStyleColor(ImGuiCol_Border, r.focused ? color::kGold : color::kPanelBorder);
  ImGui::PushStyleColor(ImGuiCol_Text, color::kText);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, color::kTextFaint);
  ImGui::SetNextItemWidth(r.max.x - S(14) - box_x);
  if (focus_text_input_) ImGui::SetKeyboardFocusHere(), focus_text_input_ = false;
  ImGui::BeginDisabled(st.extracting);
  if (st.iso_path) ImGui::InputTextWithHint("##iso", "Path to the .iso file", st.iso_path, st.iso_path_size);
  ImGui::EndDisabled();
  if (ImGui::IsItemActive()) focus_[int(page_)] = r.index;
  CountText(FontRole::kBody);
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(3);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(r.min.x, r.max.y));
  ImGui::Dummy(ImVec2(width, 0));
  if (r.focused) {
    help_title_ = "Disc image";
    help_text_ = "The path to your disc image (.iso) of the Xbox 360 disc. Type or paste it here (Enter to edit), or "
                 "choose it with Browse.";
  }
}

void LauncherMenu::DrawGameFilesRows(const LauncherStatus& st, Intent& in, float width) {
  if (st.installed && !st.show_reinstall) {
    Note("Installed in " + st.game_dir, color::kText, width);
    if (!st.unused_copy.empty())
      Note("The game files in " + st.unused_copy + " aren't used any more: delete that folder to free about 7 GB.",
           color::kWarn, width);
    if (ActionRow("##reinstall", "Reinstall from a disc image", "",
                  "Copies the game files again from your disc image, replacing the ones installed.", in, width,
                  !st.extracting))
      action_ = LauncherAction::kShowReinstall;
  } else {
    if (!st.installed) {
      Note("This port runs on files from your own copy of the game. Choose a disc image (.iso) of the Xbox 360 disc. "
           "The files (about 7 GB) are copied once into:",
           color::kText, width);
    } else {
      Note("Reinstall into:", color::kText, width);
    }
    Note(st.game_dir, color::kTextDim, width, FontRole::kSmall);
    IsoPathRow(st, in, width);
    if (ActionRow("##browse", "Browse for the disc image...", "", "Opens a file dialog to choose the .iso file.", in,
                  width, !st.extracting))
      action_ = LauncherAction::kBrowseIso;
    const bool has_path = st.iso_path && st.iso_path[0];
    if (ActionRow("##install", st.installed ? "Reinstall" : "Install", "",
                  has_path ? "Copies the game files from the disc image. Takes a minute or two."
                           : "Choose the disc image first.",
                  in, width, has_path && !st.extracting, true))
      action_ = LauncherAction::kInstall;
    if (st.extracting) {
      ProgressRow(st.extract_fraction, st.extract_progress, width);
      Note(st.extract_file, color::kTextDim, width, FontRole::kSmall);
      if (ActionRow("##cancel_install", "Cancel the install", "", "Stops copying; nothing counts as installed.", in,
                    width))
        action_ = LauncherAction::kCancelInstall;
    }
    if (st.installed && ActionRow("##keep", "Keep the installed files", "", "Back to the installed game files.", in,
                                  width, !st.extracting))
      action_ = LauncherAction::kHideReinstall;
  }
  if (!st.install_error.empty()) Note(st.install_error, color::kBad, width);
  if (st.folder_choice) {
    GroupHeading("Folder", width);
    if (st.game_dir_locked) {
      Note("The game files folder is set by a launch option.", color::kTextDim, width);
    } else {
      if (ActionRow("##folder", "Choose another folder...", "",
                    "Put the game files on another drive or in another folder. The choice is saved once that folder "
                    "holds the game.",
                    in, width, !st.extracting))
        action_ = LauncherAction::kChooseFolder;
      if (st.can_use_default &&
          ActionRow("##default_folder", "Use the default folder", "", "Back to the folder Skullburglar picks by itself.",
                    in, width, !st.extracting))
        action_ = LauncherAction::kUseDefaultFolder;
    }
    if (!st.folder_error.empty()) Note(st.folder_error, color::kBad, width);
  }
}

void LauncherMenu::DrawUpdatesRows(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in, float width) {
  for (size_t i = 0; i < settings.size(); ++i)
    if (PageOf(settings[i], sections_[i]) == LauncherPage::kUpdates) {
      if (!focus_cvar_.empty() && focus_cvar_ == settings[i].cvar) focus_target_ = row_index_, focus_cvar_.clear();
      SettingRow(settings[i], settings, in, width, false, nullptr);
    }
  Note("You have version " + st.version + ".", color::kTextDim, width);
  if (!st.update_available_feature) {
    Note("This build doesn't check for updates (a development build).", color::kTextDim, width);
    return;
  }
  if (!st.update_failed_note.empty()) Note(st.update_failed_note, color::kBad, width);
  if (st.update_checking) Note("Checking for a newer version...", color::kTextDim, width);
  if (st.update_check_failed) Note("Couldn't check for updates.", color::kTextDim, width);
  if (st.update_newer) {
    Note("Skullburglar " + st.update_latest + " is out.", color::kGold, width, FontRole::kLabel);
    if (st.update_can_install &&
        ActionRow("##update_now", "Update now", "",
                  "Downloads the new version, checks its signature and installs it, then restarts. Your settings and "
                  "saves stay.",
                  in, width, !st.update_busy, true))
      action_ = LauncherAction::kUpdateNow;
    if (st.update_can_download &&
        ActionRow("##download", "Open the download page", "", "Opens the release page in your browser.", in, width,
                  !st.update_busy))
      action_ = LauncherAction::kDownloadPage;
  } else if (!st.update_checking && !st.update_check_failed) {
    bool off = false;
    for (const auto& s : settings)
      if (s.cvar == "check_updates") off = s.value == "false";
    if (!off) Note("You have the newest version.", color::kGood, width);
  }
  if (st.update_busy) {
    char text[96];
    if (st.update_stage == 0) {
      std::snprintf(text, sizeof(text), "Downloading %.1f of %.1f MB", st.update_done_mb, st.update_total_mb);
      ProgressRow(st.update_total_mb > 0 ? float(st.update_done_mb / st.update_total_mb) : 0.0f, text, width);
      if (ActionRow("##cancel_update", "Cancel the download", "", "Stops the update; nothing changes.", in, width))
        action_ = LauncherAction::kCancelUpdate;
    } else {
      Note(st.update_stage == 1 ? "Checking the download..." : "Installing...", color::kTextDim, width);
    }
  } else if (st.update_finished) {
    Note("Updated. Restarting...", color::kGood, width);
  }
  if (!st.update_note.empty()) Note(st.update_note, st.update_note_is_error ? color::kBad : color::kTextDim, width);
}

void LauncherMenu::DrawAboutRows(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in, float width) {
  Note("Skullburglar " + st.version + ", a native port of 50 Cent: Blood on the Sand.", color::kText, width,
       FontRole::kLabel);
  Note("Settings are saved in " + st.config_path, color::kTextDim, width, FontRole::kSmall);
  if (!st.portable_note.empty()) Note(st.portable_note, color::kWarn, width, FontRole::kSmall);
  if (SavesNotice(st)) {
    GroupHeading("Your saves", width);
    if (!st.saves_warning.empty()) Note(st.saves_warning, color::kWarn, width);
    else Note(st.saves_note, st.saves_note_is_error ? color::kWarn : color::kGood, width);
    if (!st.saves_warning.empty() && st.saves_can_move) {
      if (ActionRow("##move_saves", "Move them to the user folder...", "",
                    "Moves the saves and settings from the data folder into its user folder, where the game looks for "
                    "them. Asks first; nothing is copied or deleted, and nothing already there is replaced.",
                    in, width, !st.extracting))
        OpenMoveConfirm();
      if (ActionRow("##dismiss_saves", "Not now", "", "Hides this notice until the launcher starts again. Nothing is moved.", in,
                    width))
        action_ = LauncherAction::kDismissSaves;
    }
    if (!st.saves_warning.empty() && !st.saves_note.empty()) Note(st.saves_note, color::kWarn, width);
    GroupHeading("Settings", width);
  }
  for (size_t i = 0; i < settings.size(); ++i)
    if (PageOf(settings[i], sections_[i]) == LauncherPage::kAbout) {
      if (!focus_cvar_.empty() && focus_cvar_ == settings[i].cvar) focus_target_ = row_index_, focus_cvar_.clear();
      SettingRow(settings[i], settings, in, width, false, nullptr);
      if (settings[i].cvar == "diagnostic_logging") Note("Logs are in " + st.log_folder, color::kTextDim, width, FontRole::kSmall);
    }
  GroupHeading("Reset", width);
  if (ActionRow("##reset", "Reset to defaults...", "",
                "Puts every setting back to this version's defaults. Asks first; your saves and game files are not "
                "touched.",
                in, width, !st.extracting))
    OpenResetConfirm();
  if (!st.reset_note.empty()) Note(st.reset_note, color::kGood, width);
  if (!st.save_error.empty()) Note(st.save_error, color::kBad, width);
  Note("Fonts: Inter and Oswald (SIL Open Font License 1.1). The licenses of everything included are in "
       "THIRD-PARTY-NOTICES.txt next to the program.",
       color::kTextFaint, width, FontRole::kSmall);
}

void LauncherMenu::DrawBindingRows(std::vector<Setting>& settings, Intent& in, float width) {
  const bool kbm_off = [&] {
    for (const auto& s : settings)
      if (s.cvar == "kbm") return s.value == "false";
    return false;
  }();
  Note("Each action can have several keys. Set replaces them with the next key you press, Add adds one, Default puts "
       "the action's own keys back.",
       color::kTextDim, width);
  const float keys_x = std::clamp(width * 0.38f, S(170), S(320));
  const float btn_w = S(70), btn_gap = S(6);
  const float buttons_w = 3 * btn_w + 2 * btn_gap;
  ImGui::PushID("bindings");
  for (int i = 0; i < int(settings.size()); ++i) {
    Setting& s = settings[i];
    if (!s.binding) continue;
    const bool editable = !s.locked && !kbm_off;
    ImGui::PushID(s.cvar.c_str());
    if (!focus_cvar_.empty() && focus_cvar_ == s.cvar) focus_target_ = row_index_, focus_cvar_.clear();
    const float label_w = keys_x - S(24);
    const float label_h = Measure(FontRole::kLabel, s.label, label_w).y;
    const float h = std::max(S(42), label_h + S(14));
    RowState r = BeginRow("##brow", h, width);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    TextWrapped(dl, FontRole::kLabel, ImVec2(r.min.x + S(18), r.min.y + (h - label_h) * 0.5f),
                editable ? color::kText : color::kTextFaint, s.label, label_w);
    // The keys as key caps.
    float x = r.min.x + keys_x;
    const float cap_h = S(24);
    const float keys_max = r.max.x - buttons_w - S(24);
    const auto keys = SplitKeys(s.value);
    if (capture_ == i) {
      Text(dl, FontRole::kBody, ImVec2(x, r.min.y + (h - Size(FontRole::kBody) * 1.2f) * 0.5f), color::kGold,
           "Press a key...");
    } else if (keys.empty()) {
      Text(dl, FontRole::kBody, ImVec2(x, r.min.y + (h - Size(FontRole::kBody) * 1.2f) * 0.5f), color::kTextFaint, "(none)");
    } else {
      const PromptDevice saved = device_;
      device_ = PromptDevice::kKeyboardMouse;
      for (size_t k = 0; k < keys.size(); ++k) {
        const std::string label = KeyLabel(keys[k]);
        const float w = GlyphWidth(cap_h, kKey, label.c_str());
        if (x + w > keys_max) {
          Text(dl, FontRole::kBody, ImVec2(x, r.min.y + (h - Size(FontRole::kBody) * 1.2f) * 0.5f), color::kTextDim, "+" + std::to_string(keys.size() - k));
          break;
        }
        Glyph(dl, ImVec2(x, r.min.y + (h - cap_h) * 0.5f), cap_h, kKey, label.c_str());
        x += w + S(6);
      }
      device_ = saved;
    }
    // Set / Add / Default buttons for the mouse; A / X / Y and Enter / Insert / Delete do the same.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int clicked = -1;
    static constexpr const char* kButtons[] = {"Set", "Add", "Default"};
    for (int b = 0; b < 3; ++b) {
      const ImVec2 bmin(r.max.x - S(12) - buttons_w + b * (btn_w + btn_gap), r.min.y + (h - S(28)) * 0.5f);
      const ImVec2 bmax(bmin.x + btn_w, bmin.y + S(28));
      const bool over = r.hovered && Contains(bmin, bmax, mouse);
      dl->AddRectFilled(bmin, bmax, editable ? (over ? color::kGoldDim : color::Rgb(44, 36, 28)) : color::Rgb(30, 26, 22), S(4));
      const ImVec2 ts = Measure(FontRole::kSmall, kButtons[b]);
      Text(dl, FontRole::kSmall, ImVec2((bmin.x + bmax.x - ts.x) * 0.5f, (bmin.y + bmax.y - ts.y) * 0.5f),
           editable ? color::kText : color::kTextFaint, kButtons[b]);
      if (over && r.clicked) clicked = b;
    }
    if (editable && capture_ < 0) {
      if (r.focused && in.accept) clicked = 0, in.accept = false;
      if (r.focused && in.x) clicked = 1, in.x = false;
      if (r.focused && in.y) clicked = 2, in.y = false;
      if (clicked == 0 || clicked == 1) {
        capture_ = i;
        capture_append_ = clicked == 1;
        capture_armed_frames_ = 2;  // the press that started it isn't the key
        capture_escape_held_ = -1.0f;
      } else if (clicked == 2) {
        s.value = s.default_value;
      }
    }
    if (r.focused) {
      help_title_ = s.label;
      help_text_ = "Keys: " + (keys.empty() ? std::string("none") : [&] {
        std::string t;
        for (const auto& k : keys) t += (t.empty() ? "" : ", ") + KeyLabel(k);
        return t;
      }());
      std::string d;
      for (const auto& k : SplitKeys(s.default_value)) d += (d.empty() ? "" : ", ") + KeyLabel(k);
      help_default_ = "Default: " + d;
      if (s.locked) help_locked_ = true, help_note_ = "Set by a launch option, so it can't be changed here.";
      else if (kbm_off) help_note_ = "Turn on Keyboard and mouse to change this.";
    }
    ImGui::PopID();
  }
  ImGui::PopID();
  if (ActionRow("##reset_keys", "Put every key back to its default", "",
                "Restores the default keys of every action above. Saved when you press Play.", in, width, !kbm_off)) {
    for (auto& s : settings)
      if (s.binding && !s.locked) s.value = s.default_value;
  }
}

void LauncherMenu::DrawHelp(const ImVec2& min, const ImVec2& max) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(min, max, color::kPanel, S(4));
  dl->AddRect(min, max, color::kPanelBorder, S(4), 0, std::max(1.0f, S(1)));
  const float pad = S(16);
  const float w = max.x - min.x - 2 * pad;
  float y = min.y + pad;
  const float bottom = max.y - pad;
  auto block = [&](FontRole role, ImU32 col, const std::string& text) {
    if (text.empty() || y >= bottom) return;
    const float h = Measure(role, text, w).y;
    if (y + h > bottom) ++report_.truncated;
    dl->PushClipRect(min, ImVec2(max.x, bottom + S(2)), true);
    TextWrapped(dl, role, ImVec2(min.x + pad, y), col, text, w);
    dl->PopClipRect();
    y += h + S(10);
  };
  if (help_title_.empty()) {
    block(FontRole::kBody, color::kTextDim, "Select a setting to see what it does.");
    return;
  }
  block(FontRole::kSubhead, color::kGold, Upper(help_title_));
  block(FontRole::kBody, color::kText, help_text_);
  block(FontRole::kSmall, color::kTextDim, help_default_);
  block(FontRole::kSmall, help_locked_ ? color::kWarn : color::kWarn, help_note_);
}

void LauncherMenu::DrawFooter(const LauncherStatus& st, Intent& in, float top) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float mx = std::max(S(24), size.x * 0.045f);
  dl->AddRectFilled(ImVec2(0, top), size, color::Rgb(10, 8, 7, 230));
  dl->AddLine(ImVec2(0, top), ImVec2(size.x, top), color::kPanelBorder, std::max(1.0f, S(1)));
  const float gh = S(24);
  const float cy = top + (size.y - top) * 0.5f;
  const bool kbm = device_ == PromptDevice::kKeyboardMouse;

  float left = mx;
  if (page_ != LauncherPage::kMain) {
    // Play from any page: a button, Start, or Ctrl+Enter.
    const bool can_play = st.installed && !st.extracting && !st.update_busy;
    const char* key = "Ctrl+Enter";
    const float bw = GlyphWidth(gh, kStart, key) + Measure(FontRole::kMenu, "PLAY").x + S(36);
    const ImVec2 a(left, cy - S(18)), b(left + bw, cy + S(18));
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##play_button", ImVec2(bw, b.y - a.y));
    const bool over = ImGui::IsItemHovered() && !Busy();
    if (over && ImGui::IsItemClicked(ImGuiMouseButton_Left) && can_play) action_ = LauncherAction::kPlay;
    dl->AddRectFilled(a, b, can_play ? (over ? color::kGold : color::Rgb(214, 152, 46)) : color::kTrack, S(4));
    float x = a.x + S(12);
    x += Glyph(dl, ImVec2(x, cy - gh * 0.5f), gh, kStart, key) + S(10);
    Text(dl, FontRole::kMenu, ImVec2(x, cy - Size(FontRole::kMenu) * 0.6f), can_play ? color::kOnGold : color::kTextFaint, "PLAY");
    left = b.x + S(20);
  } else {
    std::string v = "Skullburglar " + st.version;
    Text(dl, FontRole::kSmall, ImVec2(left, cy - Size(FontRole::kSmall) * 0.6f), color::kTextFaint, v);
    left += Measure(FontRole::kSmall, v).x + S(24);
  }

  // The prompts, most important first; the ones that don't fit are left out (counted in the report).
  struct Prompt {
    int button;
    const char* key;
    std::string text;
    int action;  // 0 none, 1 back, 2 accept, 3 default, 4 add key, 5 prev page, 6 next page, 7 update
    int button2 = -1;  // a second glyph before the text (LB RB)
    const char* key2 = nullptr;
  };
  std::vector<Prompt> prompts;
  const bool on_main = page_ == LauncherPage::kMain;
  if (capture_ >= 0) {
    prompts.push_back({kB, "Esc", kbm ? "Hold to cancel" : "Cancel", 0});
  } else if (on_main) {
    prompts.push_back({kA, "Enter", "Select", 2});
    if (st.update_newer && st.update_can_install && !st.update_busy) prompts.push_back({kY, "U", "Update now", 7});
    if (!kbm) prompts.push_back({kStart, "", "Play", 0});
    prompts.push_back({kRB, "E", "Settings", 6});
  } else {
    prompts.push_back({kB, "Esc", "Back", 1});
    prompts.push_back({kA, "Enter", "Select", 2});
    // Change and Default only on a setting (its help has a default); Add key on a key binding.
    const bool on_setting = !help_default_.empty();
    if (on_setting && page_ != LauncherPage::kBindings)
      prompts.push_back({kDpadLR, "\xE2\x86\x90 \xE2\x86\x92", "Change", 0});
    if (on_setting && page_ == LauncherPage::kBindings) prompts.push_back({kX, "Ins", "Add key", 4});
    if (on_setting) prompts.push_back({kY, "Del", "Default", 3});
    prompts.push_back({kLB, "Q", "Section", 6, kRB, "E"});
  }
  float x = size.x - mx;
  std::vector<std::pair<const Prompt*, float>> placed;
  auto prompt_w = [&](const Prompt& p) {
    float w = GlyphWidth(gh, p.button, p.key) + (p.text.empty() ? 0.0f : S(8) + Measure(FontRole::kBody, p.text).x);
    if (p.button2 >= 0) w += S(4) + GlyphWidth(gh, p.button2, p.key2);
    return w;
  };
  for (const auto& p : prompts) {
    const float w = prompt_w(p);
    if (x - w < left) {
      ++report_.prompts_dropped;
      continue;
    }
    x -= w;
    placed.push_back({&p, x});
    x -= S(22);
  }
  for (const auto& [p, px] : placed) {
    const float w = prompt_w(*p);
    ImGui::SetCursorScreenPos(ImVec2(px, cy - gh * 0.5f - S(4)));
    ImGui::PushID(p->key);
    ImGui::InvisibleButton("##prompt", ImVec2(w, gh + S(8)));
    const bool over = ImGui::IsItemHovered() && p->action != 0;
    if (over && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      switch (p->action) {
        case 2: in.accept = true; break;
        case 3: in.y = true; break;
        case 4: in.x = true; break;
        case 5: GoTo(NeighborPage(page_, -1)); break;
        case 6: GoTo(NeighborPage(page_, 1)); break;
        case 7: action_ = LauncherAction::kUpdateNow; break;
        default: break;
      }
      if (p->action == 1 && !Busy()) {
        GoTo(page_ == LauncherPage::kBindings ? LauncherPage::kControls : LauncherPage::kMain);
      }
    }
    ImGui::PopID();
    float gx = px + Glyph(dl, ImVec2(px, cy - gh * 0.5f), gh, p->button, p->key);
    if (p->button2 >= 0) gx += S(4) + Glyph(dl, ImVec2(gx + S(4), cy - gh * 0.5f), gh, p->button2, p->key2);
    if (!p->text.empty())
      Text(dl, FontRole::kBody, ImVec2(gx + S(8), cy - Size(FontRole::kBody) * 0.6f), over ? color::kGold : color::kText,
           p->text);
  }
}

void LauncherMenu::DrawConfirm(const LauncherStatus& st, Intent& in) {
  const bool move = confirm_move_;
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowFocus();
  ImGui::Begin("##confirm", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(0, 0), size, color::kShade);
  const float w = std::min(S(600), size.x - S(32));
  const std::string body = move ? st.saves_move_text :
      "Every setting goes back to this version's defaults: display and graphics, HDR, game, controller and co-op, and "
      "keyboard and mouse including every key binding.\n\nNot changed: the game files folder, where your data is "
      "kept, \"Check for new versions\", and anything a launch option set. Your saves are not touched.";
  const float body_h = Measure(FontRole::kBody, body, w - S(48)).y;
  const float h = S(24) + Size(FontRole::kPageTitle) * 1.3f + body_h + S(24) + S(44) + S(24);
  const ImVec2 a((size.x - w) * 0.5f, std::max(S(8), (size.y - h) * 0.5f)), b(a.x + w, a.y + h);
  dl->AddRectFilled(a, b, color::Rgb(26, 21, 17), S(6));
  dl->AddRect(a, b, color::kGoldDim, S(6), 0, std::max(1.0f, S(1)));
  float y = a.y + S(24);
  Text(dl, FontRole::kPageTitle, ImVec2(a.x + S(24), y), color::kText, move ? "MOVE YOUR SAVES AND SETTINGS?" : "RESET TO DEFAULTS?", w - S(48));
  y += Size(FontRole::kPageTitle) * 1.3f;
  TextWrapped(dl, FontRole::kBody, ImVec2(a.x + S(24), y), color::kText, body, w - S(48));
  y += body_h + S(24);
  if (in.left) confirm_focus_ = 0, in.left = false;
  if (in.right) confirm_focus_ = 1, in.right = false;
  if (in.up || in.down) in.up = in.down = false;
  bool reset = false, cancel = false;
  const char* kLabels[] = {move ? "MOVE THEM" : "RESET SETTINGS", "CANCEL"};
  float x = a.x + S(24);
  for (int i = 0; i < 2; ++i) {
    const float bw = Measure(FontRole::kMenu, kLabels[i]).x + S(36);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushID(i);
    ImGui::InvisibleButton("##confirm", ImVec2(bw, S(44)));
    const bool over = ImGui::IsItemHovered();
    if (over && in.mouse_moved) confirm_focus_ = i;
    if (over && ImGui::IsItemClicked(ImGuiMouseButton_Left)) (i == 0 ? reset : cancel) = true;
    ImGui::PopID();
    const bool f = confirm_focus_ == i;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + S(44)), f ? color::kGold : color::Rgb(44, 36, 28), S(4));
    Text(dl, FontRole::kMenu, ImVec2(x + S(18), y + (S(44) - Size(FontRole::kMenu) * 1.2f) * 0.5f),
         f ? color::kOnGold : color::kText, kLabels[i]);
    x += bw + S(16);
  }
  if (in.accept) (confirm_focus_ == 0 ? reset : cancel) = true, in.accept = false;
  if (in.back) cancel = true, in.back = false;
  ImGui::End();
  if (reset) action_ = move ? LauncherAction::kMoveSaves : LauncherAction::kResetAll;
  if (reset || cancel) confirm_reset_ = confirm_move_ = false, capture_ = -1;
}

void LauncherMenu::DrawCapture(std::vector<Setting>& settings, Intent& in) {
  (void)in;
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowFocus();
  ImGui::Begin("##capture", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(0, 0), size, color::kShade);
  const float w = std::min(S(560), size.x - S(32));
  const float h = S(200);
  const ImVec2 a((size.x - w) * 0.5f, (size.y - h) * 0.5f), b(a.x + w, a.y + h);
  dl->AddRectFilled(a, b, color::Rgb(26, 21, 17), S(6));
  dl->AddRect(a, b, color::kGoldDim, S(6), 0, std::max(1.0f, S(1)));
  Setting& s = settings[size_t(capture_)];
  Text(dl, FontRole::kPageTitle, ImVec2(a.x + S(24), a.y + S(20)), color::kText,
       capture_append_ ? "ADD A KEY" : "PRESS A KEY", w - S(48));
  Text(dl, FontRole::kLabel, ImVec2(a.x + S(24), a.y + S(20) + Size(FontRole::kPageTitle) * 1.3f), color::kGold, s.label,
       w - S(48));
  TextWrapped(dl, FontRole::kBody, ImVec2(a.x + S(24), a.y + S(110)), color::kTextDim,
              "Press a key or a mouse button, or turn the wheel. Hold Esc for a second, press B on a controller, or "
              "click Cancel to keep the keys as they are.",
              w - S(48) - S(120));
  const ImVec2 cmin(b.x - S(24) - S(110), b.y - S(24) - S(40)), cmax(b.x - S(24), b.y - S(24));
  ImGui::SetCursorScreenPos(cmin);
  ImGui::InvisibleButton("##cancel_capture", ImVec2(cmax.x - cmin.x, cmax.y - cmin.y));
  const bool over = ImGui::IsItemHovered();
  dl->AddRectFilled(cmin, cmax, over ? color::kGold : color::Rgb(44, 36, 28), S(4));
  const ImVec2 ts = Measure(FontRole::kMenu, "CANCEL");
  Text(dl, FontRole::kMenu, ImVec2((cmin.x + cmax.x - ts.x) * 0.5f, (cmin.y + cmax.y - ts.y) * 0.5f),
       over ? color::kOnGold : color::kText, "CANCEL");
  ImGui::End();

  bool cancel = over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) cancel = true;
  std::string name;
  if (capture_armed_frames_ > 0) {
    --capture_armed_frames_;
  } else if (!cancel) {
    ImGuiIO& io = ImGui::GetIO();
    // Escape: a tap binds it (it's the default Pause key), holding it cancels.
    if (ImGui::IsKeyDown(ImGuiKey_Escape)) {
      if (capture_escape_held_ < 0.0f) capture_escape_held_ = 0.0f;
      capture_escape_held_ += io.DeltaTime;
      if (capture_escape_held_ >= 0.8f) cancel = true;
    } else if (capture_escape_held_ >= 0.0f) {
      name = "Escape";
      capture_escape_held_ = -1.0f;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart && name.empty() && !cancel; ++k) {
      if (k == ImGuiKey_Escape) continue;
      if (ImGui::IsKeyPressed(ImGuiKey(k), false)) name = KeyName(ImGuiKey(k));
    }
    if (name.empty() && !cancel && !over) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) name = "LMB";
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) name = "RMB";
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) name = "MMB";
    }
    if (name.empty() && !cancel && io.MouseWheel != 0.0f) name = io.MouseWheel > 0 ? "WheelUp" : "WheelDown";
  }
  if (!name.empty()) s.value = BindKey(s.value, name, capture_append_);
  if (cancel || !name.empty()) capture_ = -1, capture_escape_held_ = -1.0f;
}

}  // namespace bots::setup
