// Shader precompile progress overlay (B-053). At launch the GPU backend compiles the pipelines
// saved by earlier runs on background threads (SDK cvar shader_precompile_background) while the
// game runs; draws that need an unfinished pipeline are skipped, so effects can be missing for a
// moment. This window says so:
//   - a centered "Compiling shaders" panel with a progress bar, until the player sends it to the
//     background (button, Enter/Space/Escape, or the pad's View/Back button). While it's up the
//     native backend holds the game at its next frame so the compile gets the whole CPU (B-098),
//     like a first-boot shader compile screen,
//   - then a small progress bar in the bottom-right corner until it finishes.
// With the new renderer (renderer_choice.h) there is no Play now: the full-screen loading screen (setup/loading_screen.h,
// the launcher's look with real progress) holds the game until the shaders are built (the port's and n1's startup step),
// then stays up while the game runs behind it until n1 draws the scene (HybridState::n1_ready), so the scene is never
// drawn by the pre-1.0 renderer meanwhile. It is up from the moment the launcher closes (or from the first frame when
// there is no launcher), and fades out over the game's first frame. BOTS_LOADING_SHOT=<dir> writes PNGs of it as it is.
// Nothing is shown if compiling finishes within kShowAfter (warm driver cache). The dialog deletes
// itself (Close) when done, so no imgui layer stays on top of the game afterwards.
// BOTS_SHADER_PROGRESS_DEMO=<seconds>: fake a precompile of that length (500 pipelines) to check the UI;
// real ones are often shorter than kShowAfter once the driver's own cache is warm.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>

#include <imgui.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "hybrid_state.h"
#include "renderer_choice.h"
#include "setup/launcher_style.h"
#include "setup/loading_screen.h"
#include "setup/setup_dialog.h"
#include "setup/update_check.h"

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/imgui_dialog.h>

class ShaderProgressDialog : public rex::ui::ImGuiDialog {
 public:
  using GraphicsGetter = std::function<rex::system::IGraphicsSystem*()>;
  using InputGetter = std::function<rex::input::InputSystem*()>;

  ShaderProgressDialog(rex::ui::ImGuiDrawer* drawer, GraphicsGetter graphics, InputGetter input)
      : ImGuiDialog(drawer), graphics_(std::move(graphics)), input_(std::move(input)) {
    if (const char* v = std::getenv("BOTS_SHADER_PROGRESS_DEMO")) demo_secs_ = std::atof(v);
    // BOTS_PRECOMPILE_HOLD=0 (scripted runs): start in the background, so the game is never held.
    if (const char* v = std::getenv("BOTS_PRECOMPILE_HOLD"); v && *v == '0') backgrounded_ = true;
    if (const char* v = std::getenv("BOTS_LOADING_SHOT"); v && *v) shot_dir_ = bots::EnvPath("BOTS_LOADING_SHOT");
  }

  // Read by the keyboard/mouse driver (bots_app.h): only the centered panel takes input; the
  // backgrounded corner bar leaves the game its pointer.
  bool captures_input() const override { return !backgrounded_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    using clk = std::chrono::steady_clock;
    rex::system::IGraphicsSystem* graphics = graphics_ ? graphics_() : nullptr;
    if (anim_start_ == clk::time_point{}) anim_start_ = clk::now();
    if (!graphics) {
      DrawBeforeGraphics(io);
      return;
    }
    if (created_ == clk::time_point{}) created_ = clk::now();
    // The renderer choice can change in the launcher after this dialog was made, so it is read once the game is starting.
    if (!hold_decided_) {
      hold_decided_ = true;
      hold_for_n1_ = !backgrounded_ && bots::UseN1();
    }
    uint32_t done = 0, total = 0;
    if (demo_secs_ > 0) {
      total = 500;
      double t = std::chrono::duration<double>(clk::now() - created_).count() / demo_secs_;
      done = uint32_t(std::min(t, 1.0) * total);
    } else {
      graphics->GetShaderPrecompileProgress(done, total);
    }
    if (hold_for_n1_) {
      DrawN1Hold(io, graphics, done, total);
      return;
    }
    // Done, or no stored pipelines at all: remove the dialog (and the imgui layer with it).
    if ((total && done >= total) || (!total && clk::now() - created_ > kNothingQueuedAfter)) {
      SetForeground(graphics, false);
      Close();
      return;
    }
    if (!total) return;
    if (first_seen_ == clk::time_point{}) first_seen_ = clk::now();
    if (clk::now() - first_seen_ < kShowAfter) return;

    float fraction = float(done) / float(total);
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%u / %u", done, total);

    if (!backgrounded_ && PadBackPressed()) backgrounded_ = true;
    // The game waits while the panel is up (backends that support it hold the guest at its next frame).
    SetForeground(graphics, !backgrounded_);
    if (!backgrounded_) {
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(ImVec2(460, 0));
      ImGui::SetNextWindowBgAlpha(0.92f);
      LauncherLook look;
      if (ImGui::Begin("Compiling shaders##bots_precompile", nullptr,
                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            "Building this GPU's shaders, once. The game starts when this finishes, or play now and let "
            "it finish in the background: effects that aren't compiled yet stay invisible until they "
            "are, and the game may run slower meanwhile.");
        ImGui::Spacing();
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(bots::setup::color::kOnGold));
        const bool play_now = ImGui::Button("Play now", ImVec2(-1, 0));
        ImGui::PopStyleColor();
        if (play_now || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
          backgrounded_ = true;
        }
        ImGui::TextDisabled("Enter / View button");
      }
      ImGui::End();
      return;
    }

    const float pad = 12.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, io.DisplaySize.y - pad), ImGuiCond_Always,
                            ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(220, 0));
    ImGui::SetNextWindowBgAlpha(0.7f);
    LauncherLook look;
    if (ImGui::Begin("##bots_precompile_mini", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
      static const char kSpinner[] = "|/-\\";
      ImGui::Text("%c Compiling shaders %d%%",
                  kSpinner[int(ImGui::GetTime() * 8.0) % 4], int(fraction * 100.0f));
      ImGui::ProgressBar(fraction, ImVec2(-1, 6), "");
    }
    ImGui::End();
  }

 private:
  // The launcher's colors for the pre-1.0 renderer's panel and corner bar (which keep the SDK's font and layout): panel,
  // border, gold bar and button instead of the SDK's green.
  struct LauncherLook {
    LauncherLook() {
      using namespace bots::setup::color;
      auto v4 = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
      ImGui::PushStyleColor(ImGuiCol_WindowBg, v4(kPanel));
      ImGui::PushStyleColor(ImGuiCol_Border, v4(kPanelBorder));
      ImGui::PushStyleColor(ImGuiCol_Text, v4(kText));
      ImGui::PushStyleColor(ImGuiCol_TextDisabled, v4(kTextDim));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(kTrack));
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, v4(kGold));
      ImGui::PushStyleColor(ImGuiCol_Button, v4(kGold));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(Rgb(246, 190, 90)));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(kGoldDim));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    }
    ~LauncherLook() {
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(9);
    }
  };

  // Seconds since the dialog first drew: the loading screen's clock (animation, estimates).
  double Now() const {
    using clk = std::chrono::steady_clock;
    return anim_start_ == clk::time_point{} ? 0.0 : std::chrono::duration<double>(clk::now() - anim_start_).count();
  }

  float DpiScale() {
    const rex::ui::Window* w = imgui_drawer() ? imgui_drawer()->window() : nullptr;
    return w ? float(w->GetDpi()) / float(w->GetMediumDpi()) : 1.0f;
  }

  // The loading screen from the moment the launcher closes (or the first frame, with no launcher) until the graphics
  // exist: the launcher and the loading screen share one backdrop, so Play leads straight into it. Desktop only: iOS,
  // tvOS and Android start elsewhere (their import screens), and show it once the graphics are up.
  void DrawBeforeGraphics(ImGuiIO& io) {
#if !(defined(__APPLE__) && TARGET_OS_IPHONE) && !defined(__ANDROID__)
    (void)io;
    if (backgrounded_ || given_up_ || !bots::UseN1() || bots::setup::LauncherOpen()) return;
    if (pre_start_ == std::chrono::steady_clock::time_point{}) pre_start_ = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - pre_start_ > kPreStartGiveUp) {
      REXLOG_WARN("loading screen: the graphics were not up after {} s; no longer drawn", kPreStartGiveUp.count());
      given_up_ = true;
      return;
    }
    shown_ = true;
    progress_.Observe(Now(), false, 0, 0, false);
    DrawScreen(1.0f);
#else
    (void)io;
#endif
  }

  void DrawScreen(float alpha) {
    const bots::setup::LoadingView view =
        bots::setup::MakeLoadingView(progress_, bots::setup::AppVersion(), Now(), DpiScale(), alpha);
    bots::setup::DrawLoadingScreen(view, bots::setup::LaunchFonts());
    LogAndShoot(view);
  }

  // One log line per ten percent of the bar (the numbers a "stuck on loading" report needs), and with BOTS_LOADING_SHOT a
  // PNG of the screen at the same moments: the first frame, each ten percent, the game phase, the fade and the last frame.
  void LogAndShoot(const bots::setup::LoadingView& view) {
    const char* phase = view.phase == bots::setup::LoadPhase::kStarting  ? "starting"
                        : view.phase == bots::setup::LoadPhase::kShaders ? "shaders"
                                                                          : "game";
    const int tenth = int(view.bar * 10.0f);
    const bool changed = tenth > logged_tenth_ || std::string(phase) != logged_phase_;
    if (changed) {
      REXLOG_INFO("loading screen: {} (shaders {} / {}, bar {}%, {}){}", phase, view.done, view.total, int(view.bar * 100.0f),
                  view.remaining_text.empty() ? std::string("no estimate yet") : view.remaining_text,
                  view.alpha < 1.0f ? ", fading out" : "");
    }
    if (!shot_dir_.empty() && (changed || (view.alpha < 1.0f && shots_ < 400))) {
      const bool skip = view.alpha < 1.0f && fade_shots_++ % 6 != 0;  // one picture in six while it fades
      if (!skip) {
        const ImGuiIO& io = ImGui::GetIO();
        bots::setup::ShotSize size;
        size.dpi = view.dpi;
        size.width = int(io.DisplaySize.x * size.dpi + 0.5f), size.height = int(io.DisplaySize.y * size.dpi + 0.5f);
        char name[96];
        std::snprintf(name, sizeof(name), "live-%03d-%s-%03d-%.0fs.png", shots_++, phase, int(view.bar * 100.0f), Now());
        std::error_code ec;
        std::filesystem::create_directories(shot_dir_, ec);
        const bool ok =
            bots::setup::WriteLoadingShot(bots::setup::EmbeddedLauncherFontData(), shot_dir_ / name, size, view);
        REXLOG_INFO("loading screen: shot {} {}x{} {}", name, size.width, size.height, ok ? "written" : "NOT WRITTEN");
      }
    }
    if (tenth > logged_tenth_) logged_tenth_ = tenth;
    logged_phase_ = phase;
  }

  // The new renderer's hold. The game waits while shaders build; after that it has to run (n1's front-end loads
  // arrive in its frames, hybrid_fill.cpp MaybeSetN1Ready), behind this screen, until n1_ready. That wait is bounded
  // by BOTS_N1_BOOT_WAIT_S; kN1ReadyTimeout only guards against n1 never reporting ready.
  void DrawN1Hold(ImGuiIO& io, rex::system::IGraphicsSystem* graphics, uint32_t done, uint32_t total) {
    (void)io;
    using clk = std::chrono::steady_clock;
    const bool compiling = total && done < total;
    const bool finished = total ? done >= total : clk::now() - created_ > kNothingQueuedAfter;
    const bots::HybridState* hs = bots::FindHybridState();
    bool ready = !hs || hs->fill.load(std::memory_order_relaxed) != 4 || hs->n1_ready.load(std::memory_order_acquire);
    if (compiling) {
      compiled_at_ = {};
    } else if (compiled_at_ == clk::time_point{}) {
      compiled_at_ = clk::now();
    }
    if (!ready && !compiling && clk::now() - compiled_at_ > kN1ReadyTimeout) {
      REXLOG_WARN("renderer hold: the new renderer did not report ready {} s after its shaders; showing the game",
                  kN1ReadyTimeout.count());
      ready = true;
    }
    progress_.Observe(Now(), true, done, total, finished);
    SetForeground(graphics, compiling);

    if (finished && ready) {
      // Handover: the game runs from here, behind a screen that fades out over its first frames (a cut would show the
      // front end's first draw popping in). Nothing was ever drawn (a warm start that was ready at once): nothing to fade.
      if (!shown_) {
        Close();
        return;
      }
      if (fade_start_ < 0) {
        fade_start_ = Now();
        REXLOG_INFO("renderer hold: done after {:.1f} s", std::chrono::duration<double>(clk::now() - created_).count());
      }
      const double t = (Now() - fade_start_) / kFadeSeconds;
      if (t >= 1.0) {
        Close();
        return;
      }
      DrawScreen(float(1.0 - t));
      return;
    }
    fade_start_ = -1;
    // Drawn while the game is held for a reason, or once it has been drawn (until the hand-over: no blank gap between the
    // phases). A start that is ready at once and has nothing to build stays quiet.
    if (!(shown_ ? true : (!ready || compiling))) return;
    shown_ = true;
    DrawScreen(1.0f);
  }

  void SetForeground(rex::system::IGraphicsSystem* graphics, bool foreground) {
    if (foreground == foreground_) return;
    foreground_ = foreground;
    graphics->SetShaderPrecompileForeground(foreground);
  }

  static constexpr std::chrono::milliseconds kShowAfter{750};
  static constexpr std::chrono::seconds kNothingQueuedAfter{20};
  static constexpr std::chrono::seconds kN1ReadyTimeout{60};
  static constexpr std::chrono::seconds kPreStartGiveUp{180};
  static constexpr double kFadeSeconds = 0.35;

  // Edge-triggered View/Back on any pad. The game sees the press too; Back does nothing on the
  // title screen and menus that appear while compiling.
  bool PadBackPressed() {
    rex::input::InputSystem* input = input_ ? input_() : nullptr;
    if (!input) return false;
    bool down = false;
    for (uint32_t user = 0; user < 4; ++user) {
      rex::input::X_INPUT_STATE state{};
      if (input->GetState(user, &state) == 0 &&
          (uint16_t(state.gamepad.buttons) & rex::input::X_INPUT_GAMEPAD_BACK)) {
        down = true;
      }
    }
    bool pressed = down && !back_was_down_;
    back_was_down_ = down;
    return pressed;
  }

  GraphicsGetter graphics_;
  InputGetter input_;
  std::chrono::steady_clock::time_point created_{};
  std::chrono::steady_clock::time_point first_seen_{};
  double demo_secs_ = 0;
  std::atomic<bool> backgrounded_ = false;  // set on the UI thread, read from input
  bool foreground_ = false;
  bool hold_for_n1_ = false;  // the new renderer: no Play now, held until n1 draws the scene
  bool shown_ = false;
  bool hold_decided_ = false, given_up_ = false;
  bots::setup::LoadingProgress progress_;
  std::chrono::steady_clock::time_point anim_start_{}, pre_start_{};
  double fade_start_ = -1;
  int logged_tenth_ = -1, shots_ = 0, fade_shots_ = 0;
  std::string logged_phase_;
  std::filesystem::path shot_dir_;  // BOTS_LOADING_SHOT: PNGs of the screen as it is (test only)
  std::chrono::steady_clock::time_point compiled_at_{};
  bool back_was_down_ = false;
};
