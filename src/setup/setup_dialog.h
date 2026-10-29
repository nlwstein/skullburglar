// Setup / launcher window (B-061). Shown before the game starts on every launch (BotsApp::
// OnFinalizePaths has the exceptions). Without game files, the player points it at their own disc
// image once and the files are copied to a per-user folder (app_paths.h), or on the desktop builds to a folder the
// player chooses, kept in bots.toml as game_data_root. The window also edits the player settings (settings.h). Play
// saves them and starts the game.
// Test hooks: BOTS_SETUP_ISO=<path> prefills the image path; BOTS_SETUP_FOLDER=<path> chooses that folder for the
// game files as "Choose another folder..." would (desktop builds); BOTS_SETUP_AUTO=1 installs (when a path is set and
// nothing is installed) and presses Play, so the flow runs unattended.
// Release builds also check for a newer version when the window opens (update_check.h).
// Controllers work too (pad_nav.h): focus starts on Play (or Browse... without game files). Start on a controller or
// Enter on the keyboard plays from anywhere in the window.
#pragma once

#include "../path_utf8.h"
#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "pad_nav.h"
#include "settings.h"
#include "firewall.h"
#include "update_check.h"

namespace bots::setup {

class SetupDialog : public rex::ui::ImGuiDialog {
 public:
  using PlayFn = std::function<void(const std::filesystem::path& game_dir)>;
  using QuitFn = std::function<void()>;

  // default_game_dir: where the game files go when the player hasn't chosen a folder (AutoGameDir).
  // game_dir_locked: the folder was given on the command line, so the window doesn't offer to change it.
  SetupDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path game_dir, std::filesystem::path default_game_dir,
              bool game_dir_locked, std::filesystem::path config_path, PlayFn play, QuitFn quit)
      : ImGuiDialog(drawer),
        game_dir_(std::move(game_dir)),
        default_game_dir_(std::move(default_game_dir)),
        game_dir_locked_(game_dir_locked),
        config_path_(std::move(config_path)),
        play_(std::move(play)),
        quit_(std::move(quit)),
        settings_(LoadSettings()),
        pad_(std::make_unique<PadNav>()) {
    installed_ = IsGameDir(game_dir_);
    if (installed_) old_game_dir_ = game_dir_;
    if (const std::string iso = bots::PathUtf8(bots::EnvPath("BOTS_SETUP_ISO")); !iso.empty()) SetIsoPath(iso);
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
    if (const std::filesystem::path folder = bots::EnvPath("BOTS_SETUP_FOLDER"); !folder.empty() && !game_dir_locked_) {
      UseGameDir(ChosenGameDir(folder, folder_error_));
    }
#endif
    const char* autorun = std::getenv("BOTS_SETUP_AUTO");
    auto_ = autorun && std::strcmp(autorun, "1") == 0;
    // Test runs: auto mode reinstalls over a game that's already there.
    const char* reinstall = std::getenv("BOTS_SETUP_REINSTALL");
    auto_reinstall_ = auto_ && reinstall && std::strcmp(reinstall, "1") == 0;
    REXLOG_INFO("setup: window open (version {}, game files {} at {}, settings file {})", AppVersion(),
                installed_ ? "installed" : "missing", bots::PathUtf8(game_dir_), bots::PathUtf8(config_path_));
    if (UpdateCheckAvailable() && Value("check_updates") == "true") StartUpdateCheck();
    StartFirewallWatch();
  }

  ~SetupDialog() override {
    firewall_->stop = true;
    CloseFirewallAsk();
    if (worker_.joinable()) {
      progress_->cancel = true;
      worker_.join();
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (pad_) pad_->Update(io);
    capturing_ = capture_ >= 0;  // as the frame starts: a key binding may take Enter this frame
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
      DrawUpdate();
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

  // The check runs on its own thread with its own state, so closing the window never waits for it.
  struct UpdateState {
    std::atomic<bool> done{false};
    UpdateInfo info;
  };

  void StartUpdateCheck() {
    update_ = std::make_shared<UpdateState>();
    std::thread([state = update_] {
      state->info = FetchUpdateInfo();
      state->done.store(true);
    }).detach();
  }

  // Windows Firewall and LAN co-op (firewall.h): re-read every 2 s while the window is open, so the line below the
  // setting follows the player's answer to Windows' prompt. A thread of its own, like the update check.
  struct FirewallWatch {
    std::atomic<bool> stop{false}, fix{false}, fixing{false}, fix_failed{false}, asked{false};
    std::atomic<int> state{int(FirewallState::kChecking)};
  };

  void StartFirewallWatch() {
    firewall_ = std::make_shared<FirewallWatch>();
#ifdef _WIN32
    std::thread([w = firewall_] {
      bool logged = false;
      while (!w->stop) {
        if (w->fix.exchange(false)) {
          w->fixing = true;
          w->fix_failed = !FixFirewall();
          w->fixing = false;
        }
        FirewallState state = CheckFirewall();
        // The Block rules of a prompt still open: show it as asking (the first check runs before the window appears).
        if (state == FirewallState::kBlocked && w->asked && FirewallPromptOpen()) state = FirewallState::kNotAsked;
        if (!logged || int(state) != w->state) REXLOG_INFO("setup: Windows Firewall for LAN co-op: {}", int(state));
        logged = true;
        w->state = int(state);
        for (int i = 0; i < 20 && !w->stop && !w->fix; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }).detach();
#else
    firewall_->state = int(FirewallState::kNotApplicable);
#endif
  }

  void DrawFirewall() {
    const auto state = FirewallState(firewall_->state.load());
    if (state == FirewallState::kNotApplicable || state == FirewallState::kChecking || state == FirewallState::kUnknown)
      return;
    if (state == FirewallState::kNotAsked && !firewall_asked_) {
      firewall_asked_ = true;
      firewall_->asked = true;
      AskFirewall();  // Windows' prompt opens now, over this window, instead of during a story
    }
    ImGui::Indent();
    const ImVec4 warn(1.0f, 0.75f, 0.3f, 1.0f);
    switch (state) {
      case FirewallState::kAllowed:
        ImGui::TextDisabled("Windows Firewall: allowed.");
        break;
      case FirewallState::kNotAsked:
        ImGui::TextColored(warn, "Windows is asking whether Skullburglar may use your network: choose Allow.");
        break;
      case FirewallState::kPublicNetwork:
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(warn,
                           "This network is set to Public, where Windows Firewall blocks LAN co-op. Set it to Private "
                           "(Settings > Network & internet > your network > Private network).");
        ImGui::PopTextWrapPos();
        break;
      case FirewallState::kBlocked:
        ImGui::TextColored(warn, "Windows Firewall is blocking LAN co-op: other players' games won't show up.");
        ImGui::BeginDisabled(firewall_->fixing.load() || firewall_->fix.load());
        if (ImGui::Button(firewall_->fixing.load() ? "Waiting for Windows..." : "Allow Skullburglar")) {
          firewall_->fix = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(firewall_->fix_failed.load() ? "Not changed. Windows asks for administrator permission."
                                                         : "Windows asks for administrator permission.");
        break;
      default:
        break;
    }
    ImGui::Unindent();
  }

  void DrawUpdate() {
    if (!update_ || !update_->done.load()) return;
    const UpdateInfo& info = update_->info;
    if (!update_logged_) {
      update_logged_ = true;
      if (!info.error.empty()) REXLOG_INFO("setup: update check failed: {}", info.error);
      else REXLOG_INFO("setup: newest release {}, this is {}", info.latest, AppVersion());
    }
    if (info.latest.empty() || !IsNewerVersion(info.latest, AppVersion())) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
    ImGui::TextWrapped("Skullburglar %s is out (you have %s).", info.latest.c_str(), AppVersion());
    ImGui::PopStyleColor();
    if (!info.page.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Download it")) OpenInBrowser(info.page);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
  }

  void DrawGameFiles() {
    ImGui::TextUnformatted("Game files");
    if (installed_ && !show_reinstall_) {
      ImGui::TextDisabled("Installed in %s", bots::PathUtf8(game_dir_).c_str());
      DrawUnusedCopy();
      if (ImGui::SmallButton("Reinstall from a disc image...")) show_reinstall_ = true;
      return;
    }
    if (!installed_) {
      ImGui::TextWrapped(
          "This port runs on files from your own copy of the game. Choose a disc image (.iso) "
          "of the Xbox 360 disc. The files (about 7 GB) are copied once into:");
    } else {
      ImGui::TextUnformatted("Reinstall into:");
    }
    ImGui::TextDisabled("%s", bots::PathUtf8(game_dir_).c_str());
    DrawFolderChoice();
    ImGui::BeginDisabled(Extracting());
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputTextWithHint("##iso", "Path to the .iso file", iso_path_, sizeof(iso_path_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
      auto picked = OpenPicker(PickIsoFile);
      if (!picked.empty()) SetIsoPath(bots::PathUtf8(picked));
    }
    if (!installed_) ImGui::SetItemDefaultFocus();
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

  // Desktop builds: the player may put the game files in another folder (a roomier drive, the install folder). The
  // choice is kept in bots.toml as game_data_root once that folder holds the game; until then the default stays.
  // iOS and tvOS have no launcher, and keep the game files in the app container.
  void DrawFolderChoice() {
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
    if (game_dir_locked_) {
      ImGui::SameLine();
      ImGui::TextDisabled("(set by a launch option)");
      return;
    }
    ImGui::BeginDisabled(Extracting());
    if (ImGui::SmallButton("Choose another folder...")) {
      const std::filesystem::path chosen = OpenPicker(PickFolder);
      if (!chosen.empty()) UseGameDir(ChosenGameDir(chosen, folder_error_));
    }
    if (game_dir_ != default_game_dir_) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Use the default folder")) UseGameDir(default_game_dir_);
    }
    ImGui::EndDisabled();
    if (!folder_error_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
      ImGui::TextWrapped("%s", folder_error_.c_str());
      ImGui::PopStyleColor();
    }
#endif
  }

  // An installed copy that the chosen folder replaced: say it can go, never delete it.
  void DrawUnusedCopy() {
    if (old_game_dir_.empty() || old_game_dir_ == game_dir_ || !IsGameDir(old_game_dir_)) return;
    ImGui::TextWrapped("The game files in %s aren't used any more: delete that folder to free about 7 GB.",
                       bots::PathUtf8(old_game_dir_).c_str());
  }

  void UseGameDir(const std::filesystem::path& dir) {
    if (dir.empty()) {  // refused; folder_error_ says why
      if (!folder_error_.empty()) REXLOG_WARN("setup: {}", folder_error_);
      return;
    }
    folder_error_.clear();
    error_.clear();
    game_dir_ = dir;
    installed_ = IsGameDir(game_dir_);
    REXLOG_INFO("setup: game folder {} ({})", bots::PathUtf8(game_dir_), installed_ ? "installed" : "missing");
    if (installed_ || game_dir_ == default_game_dir_) RememberGameDir();
  }

  // bots.toml names the folder only when it isn't the default, so a player who never chooses one keeps no entry.
  void RememberGameDir() {
    const std::string value = game_dir_ == default_game_dir_ ? std::string() : bots::PathUtf8(game_dir_);
    if (value.empty() && !ConfigHasKey(config_path_, "game_data_root")) return;  // nothing to change
    std::string err = SetConfigString(config_path_, "game_data_root", value);
    if (!err.empty()) REXLOG_WARN("setup: {}", err);
  }

  void DrawSettings() {
    ImGui::TextUnformatted("Settings");
    ImGui::TextDisabled("Saved in %s", bots::PathUtf8(config_path_).c_str());
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
      if (s.slider_max > 0) {
        float v = float(std::strtod(s.value.c_str(), nullptr));
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderFloat(s.label.c_str(), &v, float(s.slider_min), float(s.slider_max), s.slider_format.c_str(),
                               (s.slider_linear ? 0 : ImGuiSliderFlags_Logarithmic) | ImGuiSliderFlags_AlwaysClamp)) {
          char text[32];
          std::snprintf(text, sizeof(text), "%.2f", v);
          s.value = text;
        }
      } else if (s.choices.empty()) {
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
      if (s.cvar == "user_signin_live" && s.value == "true") DrawFirewall();
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
    // A controller can't press a key: B gives up, as Cancel does.
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
      capture_ = -1;
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

  // Start on a controller or Enter on the keyboard plays from anywhere in the window, except while a path is typed, a
  // list is open or a key binding waits for its key.
  bool QuickPlayPressed() const {
    if (!installed_ || Extracting() || capturing_ || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return false;
    if (pad_ && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) return true;
    return !ImGui::GetIO().WantTextInput &&
           (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
  }

  void DrawButtons() {
    const bool pad = pad_ && pad_->Connected();
    const bool ps = pad && pad_->PlayStation();
    char play[48];
    std::snprintf(play, sizeof(play), "Play  (%s)##play", pad ? (ps ? "Options" : "Start") : "Enter");
    const bool can_play = installed_ && !Extracting();
    // The way in, so it stands out from the other buttons.
    const ImVec4* colors = ImGui::GetStyle().Colors;
    ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_ButtonHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_ButtonActive]);
    ImGui::BeginDisabled(!can_play);
    const bool pressed = ImGui::Button(play, ImVec2(220, ImGui::GetFrameHeight() * 1.5f));
    if (can_play) ImGui::SetItemDefaultFocus();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (pressed || QuickPlayPressed()) {
      Play();
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(120, ImGui::GetFrameHeight() * 1.5f))) {
      if (Extracting()) progress_->cancel = true;
      pad_.reset();
      quit_();
      return;
    }
    if (!installed_) {
      ImGui::SameLine();
      ImGui::TextDisabled("Install the game files to play.");
    }
    char version[64];
    std::snprintf(version, sizeof(version), "Version %s", AppVersion());
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(version).x);
    ImGui::TextDisabled("%s", version);
    if (pad) ImGui::TextDisabled(ps ? "Controller: Cross selects, Circle goes back." : "Controller: A selects, B goes back.");
  }

  void StartExtract() {
    error_.clear();
    // Never spread the game's files into a folder that holds something else (only a chosen or configured folder
    // could; the per-user default is ours). A reinstall over the game replaces only the game's own entries.
    std::error_code ec;
    if (game_dir_ != DefaultGameDir() && !IsGameDir(game_dir_) && std::filesystem::exists(game_dir_, ec) &&
        !(std::filesystem::is_directory(game_dir_, ec) && std::filesystem::is_empty(game_dir_, ec))) {
      error_ = bots::PathUtf8(game_dir_) + " already exists and doesn't hold the game's files. Choose another folder.";
      return;
    }
    progress_ = std::make_shared<ExtractProgress>();
    result_ = std::make_shared<ExtractResult>();
    std::filesystem::path iso = bots::PathFromUtf8(iso_path_);  // the text box is UTF-8
    // Strip quotes from a pasted "C:\...\game.iso".
    std::string s = bots::PathUtf8(iso);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') iso = bots::PathFromUtf8(s.substr(1, s.size() - 2));
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
      else if (!game_dir_locked_) RememberGameDir();
    } else {
      error_ = result_->error;
    }
  }

  void RunAuto() {
    if ((!installed_ || auto_reinstall_) && !Extracting() && iso_path_[0] && error_.empty() && folder_error_.empty()) {
      auto_reinstall_ = false;
      StartExtract();
    }
    if (installed_ && !Extracting()) {
      auto_ = false;
      Play();
    }
    if (!error_.empty() || !folder_error_.empty()) {
      REXLOG_ERROR("setup: auto mode stopped: {}", error_.empty() ? folder_error_ : error_);
      auto_ = false;
    }
  }

  void Play() {
    std::string err = SaveSettings(settings_, config_path_);
    if (!err.empty()) REXLOG_WARN("setup: {}", err);
    REXLOG_INFO("setup: play with game files in {}", bots::PathUtf8(game_dir_));
    auto play = play_;
    auto dir = game_dir_;
    pad_.reset();  // the controllers are the game's from here
    Close();  // deletes this dialog; only locals below
    play(dir);
  }

  std::filesystem::path game_dir_;
  std::filesystem::path default_game_dir_;
  bool game_dir_locked_ = false;
  std::filesystem::path old_game_dir_;  // the installed copy when the window opened
  std::string folder_error_;
  std::filesystem::path config_path_;
  PlayFn play_;
  QuitFn quit_;
  std::vector<Setting> settings_;
  std::unique_ptr<PadNav> pad_;  // null once Play or Quit is pressed
  bool installed_ = false;
  bool show_reinstall_ = false;
  bool auto_ = false;
  bool auto_reinstall_ = false;
  int capture_ = -1;  // settings_ index of the binding waiting for a key, or -1
  bool capture_append_ = false, capture_armed_ = false;
  bool capturing_ = false;  // a key binding was waiting when this frame started
  char iso_path_[1024] = {};
  std::string error_;
  std::thread worker_;
  std::shared_ptr<ExtractProgress> progress_;
  std::shared_ptr<ExtractResult> result_;
  std::shared_ptr<std::atomic<bool>> done_;
  std::shared_ptr<UpdateState> update_;
  std::shared_ptr<FirewallWatch> firewall_;
  bool firewall_asked_ = false;
  bool update_logged_ = false;
};

}  // namespace bots::setup
