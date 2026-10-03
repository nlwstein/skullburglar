// Shader precompile progress overlay (B-053). At launch the GPU backend compiles the pipelines
// saved by earlier runs on background threads (SDK cvar shader_precompile_background) while the
// game runs; draws that need an unfinished pipeline are skipped, so effects can be missing for a
// moment. This window says so:
//   - a centered "Compiling shaders" panel with a progress bar, until the player sends it to the
//     background (button, Enter/Space/Escape, or the pad's View/Back button). While it's up the
//     native backend holds the game at its next frame so the compile gets the whole CPU (B-098),
//     like a first-boot shader compile screen,
//   - then a small progress bar in the bottom-right corner until it finishes.
// Nothing is shown if compiling finishes within kShowAfter (warm driver cache). The dialog deletes
// itself (Close) when done, so no imgui layer stays on top of the game afterwards.
// BOTS_SHADER_PROGRESS_DEMO=<seconds>: fake a precompile of that length (500 pipelines) to check the UI;
// real ones are often shorter than kShowAfter once the driver's own cache is warm.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include <imgui.h>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
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
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    using clk = std::chrono::steady_clock;
    rex::system::IGraphicsSystem* graphics = graphics_ ? graphics_() : nullptr;
    if (!graphics) return;
    if (created_ == clk::time_point{}) created_ = clk::now();
    uint32_t done = 0, total = 0;
    if (demo_secs_ > 0) {
      total = 500;
      double t = std::chrono::duration<double>(clk::now() - created_).count() / demo_secs_;
      done = uint32_t(std::min(t, 1.0) * total);
    } else {
      graphics->GetShaderPrecompileProgress(done, total);
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
      ImGui::SetNextWindowBgAlpha(0.85f);
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
        if (ImGui::Button("Play now", ImVec2(-1, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space) ||
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
    ImGui::SetNextWindowBgAlpha(0.55f);
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
  void SetForeground(rex::system::IGraphicsSystem* graphics, bool foreground) {
    if (foreground == foreground_) return;
    foreground_ = foreground;
    graphics->SetShaderPrecompileForeground(foreground);
  }

  static constexpr std::chrono::milliseconds kShowAfter{750};
  static constexpr std::chrono::seconds kNothingQueuedAfter{20};

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
  bool backgrounded_ = false;
  bool foreground_ = false;
  bool back_was_down_ = false;
};
