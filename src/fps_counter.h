// In-game FPS counter (B-279, GitHub #18): the `fps_counter` setting (launcher, bots.toml, REX_FPS_COUNTER=1 or
// --fps_counter) shows the frame rate, the frame time and which renderer draws, in the top-left corner, averaged over
// half a second. It is an ImGui overlay (the SDK draws it over the finished frame), so it looks the same with the new
// renderer and the pre-1.0 one. The count is of frames the renderer hands to the presenter (HybridState::swaps), not
// of overlay repaints. Off, no dialog exists and the renderer's only cost is one relaxed load per frame.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

#include <imgui.h>

#include "fps_math.h"
#include "hybrid_state.h"
#include "renderer_choice.h"

#include <rex/ui/imgui_dialog.h>

class FpsCounterDialog : public rex::ui::ImGuiDialog {
 public:
  explicit FpsCounterDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    if (bots::HybridState* hs = bots::FindHybridState()) {
      state_ = hs;
      hs->fps_counter.store(1, std::memory_order_relaxed);
    }
  }
  ~FpsCounterDialog() override {
    if (state_) state_->fps_counter.store(0, std::memory_order_relaxed);
  }

  // Takes no input: the game keeps the pad, keyboard and pointer.
  bool captures_input() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!state_) return;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    meter_.Update(state_->swaps.load(std::memory_order_relaxed), now);
    if (!meter_.has_value()) return;

    char text[64];
    std::snprintf(text, sizeof(text), "%.0f fps  %.1f ms  %s", meter_.fps(), meter_.frame_ms(),
                  bots::UseN1() ? "new" : "pre-1.0");
    // The overlay's font is a fixed pixel size: grow it with the window, so it reads the same at 720p and at 4K.
    const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f) * 1.5f;
    const float pad = 8.0f * scale;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * scale);
    if (ImGui::Begin("##bots_fps_counter", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
      ImGui::SetWindowFontScale(scale);
      ImGui::TextUnformatted(text);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  bots::HybridState* state_ = nullptr;
  bots::fps::Meter meter_{0.5};
};
