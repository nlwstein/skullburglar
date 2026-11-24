// See loading_screen.h.
#include "loading_screen.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <fstream>

#include "launcher_menu.h"
#include "ui_scale.h"

namespace bots::setup {

namespace {

LoadingLayoutReport g_report;

ImU32 Fade(ImU32 c, float alpha) {
  const int a = int(float((c >> 24) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (c & 0x00FFFFFFu) | (ImU32(a) << 24);
}

// One frame's drawing: the scale and the fonts, and the text helpers the menu has (launcher_menu.cpp), kept apart so this
// screen doesn't need the menu's state.
struct Painter {
  ImDrawList* dl = nullptr;
  const LauncherFonts* fonts = nullptr;
  float scale = 1.0f, pixel_scale = 1.0f, alpha = 1.0f;
  ImVec2 display;

  float S(float px) const { return px * scale; }
  float Size(FontRole role) const { return kRoleSize[int(role)] * scale; }
  ImU32 C(ImU32 c) const { return Fade(c, alpha); }
  ImFont* Font(FontRole role) const {
    ImFont* f = fonts && fonts->Ready() ? fonts->Font(role, pixel_scale) : nullptr;
    return f ? f : ImGui::GetFont();
  }
  ImVec2 Measure(FontRole role, const std::string& text, float wrap = 0.0f) const {
    return Font(role)->CalcTextSizeA(Size(role), FLT_MAX, wrap, text.c_str(), text.c_str() + text.size());
  }
  void Text(FontRole role, ImVec2 pos, ImU32 col, const std::string& text, float max_width = 0.0f) {
    if (text.empty()) return;
    g_report.min_text_px = std::min(g_report.min_text_px, Size(role) * (pixel_scale / scale));
    if (max_width > 0.0f && Measure(role, text).x > max_width + 0.5f) ++g_report.truncated;
    dl->AddText(Font(role), Size(role), pos, C(col), text.c_str(), text.c_str() + text.size());
  }
  float TextRight(FontRole role, float right, float y, ImU32 col, const std::string& text) {
    const float w = Measure(role, text).x;
    Text(role, ImVec2(right - w, y), col, text);
    return w;
  }
  // Wrapped text; returns its height.
  float Wrapped(FontRole role, ImVec2 pos, ImU32 col, const std::string& text, float width) {
    if (text.empty()) return 0.0f;
    g_report.min_text_px = std::min(g_report.min_text_px, Size(role) * (pixel_scale / scale));
    dl->AddText(Font(role), Size(role), pos, C(col), text.c_str(), text.c_str() + text.size(), width);
    return Measure(role, text, width).y;
  }
  void Track(ImVec2 min, ImVec2 max) { dl->AddRectFilled(min, max, C(color::kTrack), (max.y - min.y) * 0.5f); }
  void Fill(ImVec2 min, ImVec2 max, ImU32 col) { dl->AddRectFilled(min, max, C(col), (max.y - min.y) * 0.5f); }
};

const char* PhaseHeading(LoadPhase p) {
  switch (p) {
    case LoadPhase::kStarting: return "STARTING UP";
    case LoadPhase::kShaders: return "PREPARING SHADERS";
    case LoadPhase::kGame: return "LOADING THE GAME";
  }
  return "";
}

// A step's marker: a gold disc with a check when done, a ring with a pulsing dot while it runs, a faint ring waiting.
void StepMarker(Painter& p, ImVec2 c, float r, StepState state, double time) {
  const float line = std::max(1.0f, p.S(1.6f));
  switch (state) {
    case StepState::kDone:
      p.dl->AddCircleFilled(c, r, p.C(color::kGold), 24);
      p.dl->AddLine(ImVec2(c.x - r * 0.45f, c.y + r * 0.02f), ImVec2(c.x - r * 0.1f, c.y + r * 0.40f), p.C(color::kOnGold),
                    line * 1.3f);
      p.dl->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.40f), ImVec2(c.x + r * 0.50f, c.y - r * 0.36f),
                    p.C(color::kOnGold), line * 1.3f);
      break;
    case StepState::kActive: {
      const float pulse = 0.55f + 0.45f * float(0.5 + 0.5 * std::sin(time * 4.0));
      p.dl->AddCircle(c, r, p.C(color::kGold), 24, line);
      p.dl->AddCircleFilled(c, r * 0.5f, Fade(color::kGold, p.alpha * pulse), 20);
      break;
    }
    case StepState::kPending: p.dl->AddCircle(c, r, p.C(color::kTextFaint), 24, line); break;
  }
}

}  // namespace

LoadingView MakeLoadingView(const LoadingProgress& progress, const std::string& version, double time, float dpi,
                            float alpha) {
  LoadingView v;
  v.phase = progress.phase();
  v.steps = progress.steps();
  v.bar = progress.bar();
  v.determinate = progress.determinate();
  v.done = progress.done();
  v.total = progress.total();
  v.show_counts = progress.saw_compile() && progress.total() > 0;
  v.remaining_text = progress.remaining_text();
  v.first_start_hint = progress.show_first_start_hint();
  v.version = version;
  v.time = time;
  v.dpi = dpi;
  v.alpha = alpha;
  return v;
}

const LoadingLayoutReport& LastLoadingReport() { return g_report; }

void DrawLoadingScreen(const LoadingView& view, const LauncherFonts& fonts) {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 size = io.DisplaySize;
  g_report = LoadingLayoutReport();

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("##bots_loading", nullptr, flags)) {
    Painter p;
    p.dl = ImGui::GetWindowDrawList();
    p.fonts = &fonts;
    p.scale = LauncherMenuScale(size.x, size.y, std::getenv("BOTS_UI_SCALE"));
    p.pixel_scale = p.scale * std::max(view.dpi, 0.25f);
    p.alpha = std::clamp(view.alpha, 0.0f, 1.0f);
    p.display = size;
    ImDrawList* dl = p.dl;

    DrawDuskBackdrop(dl, size, false, p.alpha);

    // Title, exactly where the launcher's main menu puts it.
    const float mx = std::max(p.S(28), size.x * 0.06f);
    const float top = std::max(p.S(20), size.y * 0.07f);
    p.Text(FontRole::kTitleSmall, ImVec2(mx, top), color::kGold, "50 CENT");
    const float line1 = top + p.Size(FontRole::kTitleSmall) * 1.0f;
    p.Text(FontRole::kTitleBig, ImVec2(mx, line1), color::kText, "BLOOD ON");
    p.Text(FontRole::kTitleBig, ImVec2(mx, line1 + p.Size(FontRole::kTitleBig) * 0.92f), color::kText, "THE SAND");
    const float title_bottom = line1 + p.Size(FontRole::kTitleBig) * 2.05f;

    // The card: heading, bar, a line under it, the three steps.
    const float pw = std::clamp(size.x * 0.42f, p.S(300), p.S(560));
    const float pad = p.S(20);
    const float heading_h = p.Size(FontRole::kMenu) * 1.15f;
    const float bar_h = p.S(10);
    const float meta_h = p.Size(FontRole::kBody) * 1.3f;
    const float row_h = p.S(30);
    const float card_h = pad + heading_h + p.S(12) + bar_h + p.S(8) + meta_h + p.S(14) + 3 * row_h + pad - p.S(4);
    // Hint under the card; its room is reserved so it appearing moves nothing.
    const float hint_gap = p.S(14);
    const float hint_w = pw;
    const std::string hint =
        "The first start prepares shaders for your graphics card; later starts are fast.";
    const float hint_h = p.Measure(FontRole::kBody, hint, hint_w).y;
    const float footer = p.S(34);
    float y = std::max(title_bottom + p.S(36), size.y * 0.47f);
    y = std::min(y, size.y - footer - card_h - hint_gap - hint_h);
    y = std::max(y, title_bottom + p.S(10));
    const float x = mx;
    const ImVec2 card_min(x, y), card_max(x + pw, y + card_h);
    dl->AddRectFilled(card_min, card_max, p.C(color::kPanel), p.S(10));
    dl->AddRect(card_min, card_max, p.C(color::kPanelBorder), p.S(10), 0, std::max(1.0f, p.S(1.0f)));

    const float ix = x + pad, iw = pw - 2 * pad;
    float cy = y + pad;
    p.Text(FontRole::kMenu, ImVec2(ix, cy), color::kGold, PhaseHeading(view.phase), iw);
    cy += heading_h + p.S(12);

    // The bar.
    const ImVec2 bmin(ix, cy), bmax(ix + iw, cy + bar_h);
    p.Track(bmin, bmax);
    if (view.determinate) {
      const float f = std::clamp(view.bar, 0.0f, 1.0f);
      if (f > 0.0f) p.Fill(bmin, ImVec2(bmin.x + std::max(bar_h, iw * f), bmax.y), color::kGold);
    } else {
      // Nothing to count: a stripe sliding along the track, clipped to it.
      const float seg = iw * 0.28f;
      const float phase = float(std::fmod(view.time * 0.55, 1.0));
      const float x0 = bmin.x - seg + (iw + seg) * phase;
      const float a = std::max(x0, bmin.x), b = std::min(x0 + seg, bmax.x);
      if (b > a) p.Fill(ImVec2(a, bmin.y), ImVec2(b, bmax.y), color::kGold);
    }
    cy += bar_h + p.S(8);

    // Under the bar: the time left, and the percentage.
    std::string left;
    if (view.phase == LoadPhase::kShaders && view.show_counts) {
      left = view.remaining_text;
      if (left.empty()) left = "Estimating the time left...";
    }
    char pct[16] = "";
    if (view.determinate) std::snprintf(pct, sizeof(pct), "%d%%", int(std::clamp(view.bar, 0.0f, 1.0f) * 100.0f));
    const float pct_w = p.TextRight(FontRole::kBody, ix + iw, cy, color::kTextDim, pct);
    p.Text(FontRole::kBody, ImVec2(ix, cy), color::kTextDim, left, std::max(iw - pct_w - p.S(12), 1.0f));
    cy += meta_h + p.S(14);

    dl->AddLine(ImVec2(ix, cy - p.S(7)), ImVec2(ix + iw, cy - p.S(7)), p.C(color::kPanelBorder),
                std::max(1.0f, p.S(1.0f)));

    // The steps.
    struct Row {
      const char* label;
      StepState state;
      std::string detail;
    };
    std::string counts;
    if (view.show_counts) counts = CountText(view.done) + " / " + CountText(view.total);
    const Row rows[3] = {
        {"Starting up", view.steps.starting, ""},
        {"Preparing shaders", view.steps.shaders, view.steps.shaders == StepState::kActive ? counts : ""},
        {"Loading the game", view.steps.game, ""},
    };
    for (const Row& row : rows) {
      const float r = p.S(8);
      const ImVec2 c(ix + r, cy + row_h * 0.5f - p.S(2));
      StepMarker(p, c, r, row.state, view.time);
      const ImU32 col = row.state == StepState::kActive    ? color::kText
                        : row.state == StepState::kDone    ? color::kTextDim
                                                           : color::kTextFaint;
      const float tx = ix + r * 2 + p.S(14);
      const float text_h = p.Size(FontRole::kLabel);
      float dw = 0.0f;
      if (!row.detail.empty()) dw = p.TextRight(FontRole::kBody, ix + iw, c.y - p.Size(FontRole::kBody) * 0.5f, color::kTextDim, row.detail);
      p.Text(FontRole::kLabel, ImVec2(tx, c.y - text_h * 0.5f - p.S(1)), col, row.label,
             std::max(iw - (tx - ix) - dw - p.S(12), 1.0f));
      cy += row_h;
    }

    if (view.first_start_hint) p.Wrapped(FontRole::kBody, ImVec2(x, y + card_h + hint_gap), color::kTextDim, hint, hint_w);

    if (!view.version.empty())
      p.Text(FontRole::kSmall, ImVec2(mx, size.y - p.S(26) - p.Size(FontRole::kSmall) * 0.5f), color::kTextFaint,
             "Version " + view.version, size.x * 0.5f);

    g_report.inside = card_max.y + (view.first_start_hint ? hint_gap + hint_h : 0.0f) <= size.y && card_max.x <= size.x;
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}

std::vector<LoadingScenario> LoadingScenarios(const std::string& version) {
  std::vector<LoadingScenario> out;
  auto add = [&](const char* name, double t_compile, uint32_t done, uint32_t total, bool finished, double game_secs,
                 double fade_alpha) {
    // Replay a start through the same logic the game uses: one sample a second.
    LoadingProgress p;
    p.Observe(0.0, false, 0, 0, false);
    double now = 0.0;
    if (total > 0 || finished) {
      const double rate = t_compile > 0 ? double(done) / t_compile : 0;
      for (double t = 1.0; t <= t_compile; t += 1.0) {
        now = t;
        p.Observe(now, true, uint32_t(rate * t), total, false);
      }
      now = t_compile;
      p.Observe(now, true, done, total, finished);
      for (double t = 1.0; t <= game_secs; t += 1.0) p.Observe(now + t, true, done, total, finished);
      now += game_secs;
    } else {
      p.Observe(0.5, true, 0, 0, false);
      now = 0.5;
    }
    LoadingScenario s;
    s.name = name;
    s.view = MakeLoadingView(p, version, now, 1.0f, float(fade_alpha));
    out.push_back(std::move(s));
  };
  LoadingProgress start;
  start.Observe(0.2, false, 0, 0, false);
  out.push_back({"01-starting", MakeLoadingView(start, version, 0.2, 1.0f, 1.0f)});
  add("02-shaders-early", 14, 160, 2686, false, 0, 1.0);
  add("03-shaders-mid", 120, 1343, 2686, false, 0, 1.0);
  add("04-shaders-late", 215, 2540, 2686, false, 0, 1.0);
  add("05-loading-game", 225, 2686, 2686, true, 4, 1.0);
  add("06-handover", 225, 2686, 2686, true, 12, 0.45);
  // A warm start: nothing to build, so a stripe.
  {
    LoadingProgress warm;
    warm.Observe(0.0, false, 0, 0, false);
    warm.Observe(2.0, true, 120, 120, true);
    out.push_back({"07-warm-start", MakeLoadingView(warm, version, 3.0, 1.0f, 1.0f)});
  }
  return out;
}

bool WriteLoadingShot(const LauncherFontData& font_data, const std::filesystem::path& file, const ShotSize& size,
                      LoadingView view) {
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGuiContext* ctx = ImGui::CreateContext();
  ImGui::SetCurrentContext(ctx);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  const float logical_w = float(size.width) / size.dpi, logical_h = float(size.height) / size.dpi;
  io.DisplaySize = ImVec2(logical_w, logical_h);
  io.Fonts->AddFontDefault();
  LauncherFonts fonts;
  fonts.Build(io.Fonts, font_data, {LauncherMenuScale(logical_w, logical_h, std::getenv("BOTS_UI_SCALE")) * size.dpi});
  unsigned char* pixels = nullptr;
  int tw = 0, th = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th);
  io.Fonts->SetTexID(ImTextureID(1));
  view.dpi = size.dpi;
  for (int i = 0; i < 2; ++i) {
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    DrawLoadingScreen(view, fonts);
    ImGui::Render();
  }
  std::vector<uint8_t> rgb(size_t(size.width) * size_t(size.height) * 3, 0);
  RasterizeDrawData(ImGui::GetDrawData(), size.dpi, size.width, size.height, io.Fonts->TexData, rgb);
  const bool ok = WritePng(file, size.width, size.height, rgb);
  ImGui::DestroyContext(ctx);
  ImGui::SetCurrentContext(previous);
  return ok;
}

int WriteLoadingScenarioShots(const LauncherFontData& fonts, const std::filesystem::path& dir,
                              const std::vector<ShotSize>& sizes, const std::string& version,
                              const std::function<void(const std::string&)>& log) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream report(dir / "shots.txt", std::ios::app);
  int problems = 0;
  for (const LoadingScenario& s : LoadingScenarios(version)) {
    for (const ShotSize& size : sizes) {
      const bool ok = WriteLoadingShot(fonts, dir / (s.name + "-" + size.Name() + ".png"), size, s.view);
      const LoadingLayoutReport& r = LastLoadingReport();
      char line[256];
      std::snprintf(line, sizeof(line), "shot %s %s: cut-off text %d, %s, smallest text %.1f px%s", s.name.c_str(),
                    size.Name().c_str(), r.truncated, r.inside ? "inside" : "OUTSIDE", double(r.min_text_px),
                    ok ? "" : " (NOT WRITTEN)");
      if (log) log(line);
      if (report) report << line << "\n";
      if (!ok || r.truncated || !r.inside) ++problems;
    }
  }
  return problems;
}

}  // namespace bots::setup
