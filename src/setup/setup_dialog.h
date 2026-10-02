// First-run setup / launcher window (B-061). Shown before the game starts when no game files are
// installed, or on request (--launcher). The player points it at their own disc image once. The
// files are copied to a per-user folder (app_paths.h), so later launches go straight to the game.
// The window also edits the player settings (settings.h). Play saves them and starts the game.
// Test hooks: BOTS_SETUP_ISO=<path> prefills the image path; BOTS_SETUP_AUTO=1 installs (when a
// path is set and nothing is installed) and presses Play, so the flow runs unattended.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "app_paths.h"
#include "file_picker.h"
#include "iso_extract.h"
#include "settings.h"

namespace bots::setup {

class SetupDialog : public rex::ui::ImGuiDialog {
 public:
  using PlayFn = std::function<void(const std::filesystem::path& game_dir)>;
  using QuitFn = std::function<void()>;

  SetupDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path game_dir,
              std::filesystem::path config_path, PlayFn play, QuitFn quit)
      : ImGuiDialog(drawer),
        game_dir_(std::move(game_dir)),
        config_path_(std::move(config_path)),
        play_(std::move(play)),
        quit_(std::move(quit)),
        settings_(LoadSettings()) {
    installed_ = IsGameDir(game_dir_);
    if (const char* iso = std::getenv("BOTS_SETUP_ISO"); iso && *iso) SetIsoPath(iso);
    const char* autorun = std::getenv("BOTS_SETUP_AUTO");
    auto_ = autorun && std::strcmp(autorun, "1") == 0;
    REXLOG_INFO("setup: window open (game files {} at {}, settings file {})",
                installed_ ? "installed" : "missing", game_dir_.string(), config_path_.string());
  }

  ~SetupDialog() override {
    if (worker_.joinable()) {
      progress_->cancel = true;
      worker_.join();
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    PollWorker();
    if (auto_) RunAuto();

    const float width = std::min(720.0f, io.DisplaySize.x - 40.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(0.95f);
    if (ImGui::Begin("50 Cent - Blood on the Sand##bots_setup", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      DrawGameFiles();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      DrawSettings();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      DrawButtons();
    }
    ImGui::End();
  }

 private:
  void SetIsoPath(const std::string& path) {
    std::snprintf(iso_path_, sizeof(iso_path_), "%s", path.c_str());
  }

  bool Extracting() const { return worker_.joinable(); }

  void DrawGameFiles() {
    ImGui::TextUnformatted("Game files");
    if (installed_ && !show_reinstall_) {
      ImGui::TextDisabled("Installed in %s", game_dir_.string().c_str());
      if (ImGui::SmallButton("Reinstall from a disc image...")) show_reinstall_ = true;
      return;
    }
    if (!installed_) {
      ImGui::TextWrapped(
          "This port runs on files from your own copy of the game. Choose a disc image (.iso) "
          "of the Xbox 360 disc. The files (about 7 GB) are copied once into:");
      ImGui::TextDisabled("%s", game_dir_.string().c_str());
    }
    ImGui::BeginDisabled(Extracting());
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputTextWithHint("##iso", "Path to the .iso file", iso_path_, sizeof(iso_path_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
      auto picked = PickIsoFile();
      if (!picked.empty()) SetIsoPath(picked.string());
    }
    ImGui::BeginDisabled(iso_path_[0] == 0);
    if (ImGui::Button(installed_ ? "Reinstall" : "Install", ImVec2(160, 0))) StartExtract();
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (Extracting()) {
      uint64_t done = progress_->bytes_done, total = progress_->bytes_total;
      float fraction = total ? float(double(done) / double(total)) : 0.0f;
      char overlay[96];
      std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f GB", done / 1e9, total / 1e9);
      ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);
      ImGui::TextDisabled("File %u of %u: %s", progress_->files_done.load(),
                          progress_->files_total.load(), progress_->Current().c_str());
      if (ImGui::Button("Cancel")) progress_->cancel = true;
    }
    if (!error_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
      ImGui::TextWrapped("%s", error_.c_str());
      ImGui::PopStyleColor();
    }
  }

  void DrawSettings() {
    ImGui::TextUnformatted("Settings");
    ImGui::TextDisabled("Saved in %s", config_path_.string().c_str());
    for (auto& s : settings_) {
      ImGui::PushID(s.cvar.c_str());
      ImGui::BeginDisabled(s.locked || Extracting());
      if (s.choices.empty()) {
        bool on = s.value == "true";
        if (ImGui::Checkbox(s.label.c_str(), &on)) s.value = on ? "true" : "false";
      } else {
        auto it = std::find_if(s.choices.begin(), s.choices.end(),
                               [&](const Choice& c) { return c.value == s.value; });
        std::string preview = it != s.choices.end() ? it->label : s.value;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo(s.label.c_str(), preview.c_str())) {
          for (const auto& c : s.choices) {
            if (ImGui::Selectable(c.label.c_str(), c.value == s.value)) s.value = c.value;
          }
          ImGui::EndCombo();
        }
      }
      ImGui::EndDisabled();
      if (s.locked) {
        ImGui::SameLine();
        ImGui::TextDisabled("(set by a launch option)");
      } else if (!s.help.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", s.help.c_str());
      }
      ImGui::PopID();
    }
  }

  void DrawButtons() {
    ImGui::BeginDisabled(!installed_ || Extracting());
    if (ImGui::Button("Play", ImVec2(160, 0))) Play();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(120, 0))) {
      if (Extracting()) progress_->cancel = true;
      quit_();
    }
    if (!installed_) {
      ImGui::SameLine();
      ImGui::TextDisabled("Install the game files to play.");
    }
  }

  void StartExtract() {
    error_.clear();
    progress_ = std::make_shared<ExtractProgress>();
    result_ = std::make_shared<ExtractResult>();
    std::filesystem::path iso = iso_path_;
    // Strip quotes from a pasted "C:\...\game.iso".
    std::string s = iso.string();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') iso = s.substr(1, s.size() - 2);
    done_ = std::make_shared<std::atomic<bool>>(false);
    worker_ = std::thread([iso, dest = game_dir_, progress = progress_, result = result_,
                           done = done_]() {
      *result = ExtractIso(iso, dest, *progress);
      done->store(true);
    });
  }

  void PollWorker() {
    if (!Extracting() || !done_->load()) return;
    worker_.join();
    if (result_->ok) {
      installed_ = IsGameDir(game_dir_);
      show_reinstall_ = false;
      if (!installed_) error_ = "Extraction finished but default.xex or C1Game is missing.";
    } else {
      error_ = result_->error;
    }
  }

  void RunAuto() {
    if (!installed_ && !Extracting() && iso_path_[0] && error_.empty()) StartExtract();
    if (installed_ && !Extracting()) {
      auto_ = false;
      Play();
    }
    if (!error_.empty()) {
      REXLOG_ERROR("setup: auto mode stopped: {}", error_);
      auto_ = false;
    }
  }

  void Play() {
    std::string err = SaveSettings(settings_, config_path_);
    if (!err.empty()) REXLOG_WARN("setup: {}", err);
    REXLOG_INFO("setup: play with game files in {}", game_dir_.string());
    auto play = play_;
    auto dir = game_dir_;
    Close();  // deletes this dialog; only locals below
    play(dir);
  }

  std::filesystem::path game_dir_;
  std::filesystem::path config_path_;
  PlayFn play_;
  QuitFn quit_;
  std::vector<Setting> settings_;
  bool installed_ = false;
  bool show_reinstall_ = false;
  bool auto_ = false;
  char iso_path_[1024] = {};
  std::string error_;
  std::thread worker_;
  std::shared_ptr<ExtractProgress> progress_;
  std::shared_ptr<ExtractResult> result_;
  std::shared_ptr<std::atomic<bool>> done_;
};

}  // namespace bots::setup
