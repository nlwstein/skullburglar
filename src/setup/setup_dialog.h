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
      if (s.binding) continue;  // DrawBindings
      if (!s.section.empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText(s.section.c_str());
      }
      ImGui::PushID(s.cvar.c_str());
      // The rest of the keyboard and mouse section only matters with it on.
      const bool kbm_off = s.cvar.starts_with("kbm_") && Value("kbm") == "false";
      ImGui::BeginDisabled(s.locked || Extracting() || kbm_off);
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
    DrawBindings();
  }

  std::string Value(const std::string& cvar) const {
    for (const auto& s : settings_)
      if (s.cvar == cvar) return s.value;
    return {};
  }

  // Key bindings (kbm_* cvars): each action lists its keys. Set waits for the next key, mouse button or wheel
  // notch and makes it the only key; + adds one; Reset restores the default.
  void DrawBindings() {
    if (std::none_of(settings_.begin(), settings_.end(), [](const Setting& s) { return s.binding; })) return;
    ImGui::BeginDisabled(Value("kbm") == "false" || Extracting());
    if (ImGui::TreeNode("Key bindings")) {
      if (capture_ >= 0) PollCapture();
      if (ImGui::BeginTable("##bindings", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                            ImVec2(0, 240))) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        for (int i = 0; i < int(settings_.size()); ++i) {
          Setting& s = settings_[i];
          if (!s.binding) continue;
          ImGui::PushID(s.cvar.c_str());
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(s.label.c_str());
          ImGui::TableNextColumn();
          if (capture_ == i) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press a key or mouse button...");
          } else {
            std::string keys = s.value;
            for (size_t at = keys.find(','); at != std::string::npos; at = keys.find(',', at + 2)) keys.insert(at + 1, " ");
            ImGui::TextUnformatted(keys.empty() ? "(none)" : keys.c_str());
          }
          ImGui::TableNextColumn();
          ImGui::BeginDisabled(s.locked);
          if (capture_ == i) {
            if (ImGui::SmallButton("Cancel")) capture_ = -1;
          } else {
            if (ImGui::SmallButton("Set")) StartCapture(i, false);
            ImGui::SameLine();
            if (ImGui::SmallButton("+")) StartCapture(i, true);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) s.value = s.default_value;
          }
          ImGui::EndDisabled();
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (ImGui::SmallButton("Reset all keys")) {
        for (auto& s : settings_)
          if (s.binding && !s.locked) s.value = s.default_value;
      }
      ImGui::TreePop();
    } else {
      capture_ = -1;
    }
    ImGui::EndDisabled();
  }

  void StartCapture(int index, bool append) {
    capture_ = index;
    capture_append_ = append;
    capture_armed_ = false;  // from the next frame, so the click on Set isn't taken as the key
  }

  // Key names as the kbm_* cvars spell them (rex/ui/keybinds.cpp).
  static std::string KeyName(ImGuiKey key) {
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

  void PollCapture() {
    if (!capture_armed_) {
      capture_armed_ = true;
      return;
    }
    std::string name;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && name.empty(); ++k) {
      if (ImGui::IsKeyPressed(ImGuiKey(k), false)) name = KeyName(ImGuiKey(k));
    }
    // Mouse buttons count unless they are clicking a button in this window (Cancel, another row).
    if (name.empty() && !ImGui::IsAnyItemHovered()) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) name = "LMB";
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) name = "RMB";
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) name = "MMB";
    }
    if (name.empty() && ImGui::GetIO().MouseWheel != 0) name = ImGui::GetIO().MouseWheel > 0 ? "WheelUp" : "WheelDown";
    if (name.empty()) return;
    std::string& value = settings_[capture_].value;
    if (!capture_append_ || value.empty()) {
      value = name;
    } else if (("," + value + ",").find("," + name + ",") == std::string::npos) {
      value += "," + name;
    }
    capture_ = -1;
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
  int capture_ = -1;  // settings_ index of the binding waiting for a key, or -1
  bool capture_append_ = false, capture_armed_ = false;
  char iso_path_[1024] = {};
  std::string error_;
  std::thread worker_;
  std::shared_ptr<ExtractProgress> progress_;
  std::shared_ptr<ExtractResult> result_;
  std::shared_ptr<std::atomic<bool>> done_;
};

}  // namespace bots::setup
