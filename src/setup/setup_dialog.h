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
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>

#include "app_paths.h"
#include "portable.h"
#include "file_picker.h"
#include "iso_extract.h"
#include "pad_nav.h"
#include "settings.h"
#include "settings_logic.h"
#include "ui_scale.h"
#include "firewall.h"
#include "update_check.h"
#include "update_install.h"

namespace bots::setup {

// TextDisabled / TextColored that wrap at the window's edge: a long folder or note used to run past the edge and was cut
// off (B-387). Not for text after SameLine or in windows that size themselves to their text (the F6 window).
inline void WrappedDisabled(const char* fmt, ...) IM_FMTARGS(1);
inline void WrappedDisabled(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrappedV(fmt, args);
  ImGui::PopStyleColor();
  va_end(args);
}
inline void WrappedColored(const ImVec4& color, const char* fmt, ...) IM_FMTARGS(2);
inline void WrappedColored(const ImVec4& color, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextWrappedV(fmt, args);
  ImGui::PopStyleColor();
  va_end(args);
}

// A setting's help in a tooltip that wraps (30 text heights wide, at most half the display) instead of running off the
// screen as one line.
inline void HelpTooltip(const std::string& text) {
  if (!ImGui::BeginTooltip()) return;
  ImGui::PushTextWrapPos(std::min(ImGui::GetFontSize() * 30.0f, ImGui::GetIO().DisplaySize.x * 0.5f));
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

inline bool DrawSetting(Setting& s, float item_width = 260.0f) {
  bool changed = false;
  ImGui::PushID(s.cvar.c_str());
  ImGui::BeginDisabled(s.locked);
  if (s.slider_max > 0) {
    float v = float(std::strtod(s.value.c_str(), nullptr));
    ImGui::SetNextItemWidth(item_width);
    changed = ImGui::SliderFloat(s.label.c_str(), &v, float(s.slider_min), float(s.slider_max), s.slider_format.c_str(),
                                 (s.slider_linear ? 0 : ImGuiSliderFlags_Logarithmic) | ImGuiSliderFlags_AlwaysClamp);
    if (changed) {
      char text[32];
      std::snprintf(text, sizeof(text), s.cvar == "vulkan_hdr_middle_gray" ? "%.3f" : "%.2f", v);
      s.value = text;
    }
  } else if (s.choices.empty()) {
    bool on = s.value == "true";
    changed = ImGui::Checkbox(s.label.c_str(), &on);
    if (changed) s.value = on ? "true" : "false";
  } else {
    std::string preview = s.value;
    for (const auto& c : s.choices) if (c.value == s.value) preview = c.label;
    ImGui::SetNextItemWidth(item_width);
    if (ImGui::BeginCombo(s.label.c_str(), preview.c_str())) {
      for (const auto& c : s.choices) {
        if (ImGui::Selectable(c.label.c_str(), c.value == s.value)) s.value = c.value, changed = true;
      }
      ImGui::EndCombo();
    }
  }
  ImGui::EndDisabled();
  if (s.locked) {
    ImGui::SameLine();
    ImGui::TextDisabled("(set by a launch option)");
  } else if (!s.help.empty() && ImGui::IsItemHovered()) {
    HelpTooltip(s.help);
  }
  ImGui::PopID();
  return changed;
}

class SetupDialog : public rex::ui::ImGuiDialog {
 public:
  using PlayFn = std::function<void(const std::filesystem::path& game_dir)>;
  using QuitFn = std::function<void()>;

  // default_game_dir: where the game files go when the player hasn't chosen a folder (AutoGameDir).
  // game_dir_locked: the folder was given on the command line, so the window doesn't offer to change it.
  SetupDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path game_dir, std::filesystem::path default_game_dir,
              bool game_dir_locked, std::filesystem::path config_path, PlayFn play, QuitFn quit)
      : ImGuiDialog(drawer),
        game_dir_(bots::LongPath(game_dir)),
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
    // Test runs: press "Reset to defaults" as the window opens (the window can't be clicked from a script).
    if (const char* reset = std::getenv("BOTS_SETUP_RESET"); reset && std::strcmp(reset, "1") == 0) {
      int changed = 0;
      std::string err = ResetSettings(settings_, config_path_, &changed);
      REXLOG_INFO("setup: BOTS_SETUP_RESET: {} settings changed{}{}", changed, err.empty() ? "" : ", error: ", err);
    }
    const char* autorun = std::getenv("BOTS_SETUP_AUTO");
    auto_ = autorun && std::strcmp(autorun, "1") == 0;
    // Test runs: auto mode reinstalls over a game that's already there.
    const char* reinstall = std::getenv("BOTS_SETUP_REINSTALL");
    auto_reinstall_ = auto_ && reinstall && std::strcmp(reinstall, "1") == 0;
    REXLOG_INFO("setup: window open (version {}, game files {} at {}, settings file {})", AppVersion(),
                installed_ ? "installed" : "missing", bots::PathUtf8(game_dir_), bots::PathUtf8(config_path_));
    if (UpdateCheckAvailable() && Value("check_updates") == "true") StartUpdateCheck();
    StartFirewallWatch();
    misplaced_ = FindMisplacedGameData(ActiveDataRoot());
    if (!misplaced_.empty()) REXLOG_WARN("setup: {}", MisplacedMessage(ActiveDataRoot(), misplaced_));
    if (misplaced_.empty()) misplaced_note_ = ActiveMarkerHint();  // B-389: portable.txt misnamed, shown in the same place
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

    // Text and layout size (ui_scale.h): the style is scaled for this window only and put back at the end, so the
    // game's overlays (FPS counter, shader progress) keep their own size.
    ui_scale_ = LauncherUiScale(io.DisplaySize.y, std::getenv("BOTS_UI_SCALE"));
    const ImGuiStyle unscaled_style = ImGui::GetStyle();
    ImGui::GetStyle().ScaleAllSizes(ui_scale_);
    ImGui::GetStyle().FontScaleMain *= ui_scale_;

    const float width = LauncherWindowWidth(io.DisplaySize.x, ui_scale_);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // The settings scroll inside the window and the buttons stay at the bottom, so at a larger text size Play is
    // always in view (it was the last line of one long scrolling window).
    ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y - S(20.0f)));
    ImGui::SetNextWindowBgAlpha(0.95f);
    if (ImGui::Begin("50 Cent - Blood on the Sand##bots_setup", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      LogUiScale(io);  // before the draw calls: Play can delete this dialog
      DrawMisplaced();
      DrawGameFiles();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      // The footer's height is what the last frame measured (an update notice makes it taller); 0 on the first.
      // Kept in ImGui's window storage, not a member: Play can delete this dialog inside DrawButtons.
      ImGuiStorage* storage = ImGui::GetStateStorage();
      const ImGuiID footer_id = ImGui::GetID("footer_height");
      if (ImGui::BeginChild("##settings", ImVec2(0, -storage->GetFloat(footer_id, 0.0f)), ImGuiChildFlags_None)) {
        DrawSettings();
        settings_overflow_ = ImGui::GetScrollMaxX();
      }
      ImGui::EndChild();
      const float footer_top = ImGui::GetCursorPosY();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      DrawButtons();
      storage->SetFloat(footer_id, ImGui::GetCursorPosY() - footer_top + ImGui::GetStyle().WindowPadding.y);
      // In the window's storage, not a member: Play can delete this dialog inside DrawButtons.
      storage->SetFloat(ImGui::GetID("window_overflow"), ImGui::GetScrollMaxX());
    }
    ImGui::End();
    ImGui::GetStyle() = unscaled_style;
  }

 private:
  // Scaled pixels: a size from the layout (designed at scale 1) at this window's text scale.
  float S(float pixels) const { return pixels * ui_scale_; }

  // One log line per change of scale or window size: the numbers a "text too small" report needs.
  void LogUiScale(const ImGuiIO& io) {
    const float dpi = float(imgui_drawer()->window()->GetDpi());
    const float font = ImGui::GetFontSize();
    // The overflow is last frame's: how far the settings list and the window's own content run past their right edge (0 =
    // everything fits; the layout wraps long lines, so more than 0 means something doesn't).
    const float window_overflow = ImGui::GetStateStorage()->GetFloat(ImGui::GetID("window_overflow"), 0.0f);
    const float overflow = std::max(settings_overflow_, window_overflow);
    if (io.DisplaySize.x == logged_width_ && io.DisplaySize.y == logged_height_ && ui_scale_ == logged_scale_ &&
        font == logged_font_ && overflow == logged_overflow_)
      return;
    logged_width_ = io.DisplaySize.x, logged_height_ = io.DisplaySize.y, logged_scale_ = ui_scale_, logged_font_ = font;
    logged_overflow_ = overflow;
    REXLOG_INFO("setup: UI scale {:.2f} (window {:.0f}x{:.0f} logical, display scaling {:.0f}%, text {:.1f} px, "
                "about {:.1f} px on screen)",
                ui_scale_, io.DisplaySize.x, io.DisplaySize.y, dpi * 100.0f / 96.0f, font, font * dpi / 96.0f);
    REXLOG_INFO("setup: layout: launcher {:.0f} px wide, content runs {:.0f} px past the right edge (settings {:.0f}, "
                "window {:.0f})",
                LauncherWindowWidth(io.DisplaySize.x, ui_scale_), overflow, settings_overflow_, window_overflow);
  }

  void SetIsoPath(const std::string& path) {
    std::snprintf(iso_path_, sizeof(iso_path_), "%s", path.c_str());
  }

  bool Extracting() const { return worker_.joinable(); }

  // The check runs on its own thread with its own state, so closing the window never waits for it.
  struct UpdateState {
    std::atomic<bool> done{false};
    UpdateInfo info;
    // "Update now" (update_install.h): what this install is, its package, and the install thread's state.
    InstallKind kind = InstallKind::kNone;
    Asset asset;
    bool has_asset = false;
    bool needs_move = false;  // macOS: run from a disk image or App Translocation, so it can't replace itself
    std::string blocker;  // why there's no Update now button, when there's a package for it
    std::string failed_note;  // Windows: the last update's setup program never finished
    InstallProgress progress;
    std::atomic<bool> running{false}, finished{false};  // finished: installed, the program should quit now
    std::mutex mutex;
    std::string install_error;  // guarded by mutex
  };

  void StartUpdateCheck() {
    update_ = std::make_shared<UpdateState>();
    std::thread([state = update_] {
      CleanUpOldUpdates();
      state->failed_note = TakeFailedUpdateNote();
      state->info = FetchUpdateInfo();
      if (IsNewerVersion(state->info.latest, AppVersion())) {
        state->kind = CurrentInstallKind();
        state->needs_move = state->kind == InstallKind::kNone && MacAppNeedsMove(PathToUtf8(SelfExeFolder()));
        // Only a signed listing gets the button (an unsigned one, or a build without the key, keeps "Download it").
        const Asset* a = PickAsset(state->info, state->kind);
        if (a && !a->signed_ok) {
          REXLOG_INFO("setup: the release listing isn't signed for this build; Update now stays off");
          a = nullptr;
        }
        if (a) {
          state->asset = *a;
          state->has_asset = true;
          state->blocker = SelfUpdateBlocker(state->kind);
        }
      }
      state->done.store(true);
    }).detach();
  }

  bool UpdateBusy() const { return update_ && update_->running.load(); }
  // Past the download the swap can't be interrupted: Quit waits.
  bool UpdateCommitted() const {
    return UpdateBusy() && update_->progress.stage.load() > int(UpdateStage::kDownloading);
  }

  void StartInstall() {
    auto state = update_;
    if (!state || state->running.exchange(true)) return;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->install_error.clear();
    }
    state->progress.cancel = false;
    if (!settings_file_moved_) SaveSettings(settings_, config_path_);  // what was changed in this window survives the restart
    std::thread([state] {
      std::string error;
      bool ok = InstallUpdate(state->kind, state->asset, state->progress, error);
      if (!ok) {
        REXLOG_WARN("setup: update failed: {}", error);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->install_error = error == "cancelled" ? std::string() : error;
      } else {
        REXLOG_INFO("setup: updated to {}; restarting", state->info.latest);
      }
      state->finished.store(ok);
      state->running.store(false);
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
        WrappedDisabled("Windows Firewall: allowed.");
        break;
      case FirewallState::kNotAsked:
        WrappedColored(warn, "Windows is asking whether Skullburglar may use your network: choose Allow.");
        break;
      case FirewallState::kPublicNetwork:
        WrappedColored(warn,
                       "This network is set to Public, where Windows Firewall blocks LAN co-op. Set it to Private "
                       "(Settings > Network & internet > your network > Private network).");
        break;
      case FirewallState::kBlocked:
        WrappedColored(warn, "Windows Firewall is blocking LAN co-op: other players' games won't show up.");
        ImGui::BeginDisabled(firewall_->fixing.load() || firewall_->fix.load());
        if (ImGui::Button(firewall_->fixing.load() ? "Waiting for Windows..." : "Allow Skullburglar")) {
          firewall_->fix = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        WrappedDisabled(firewall_->fix_failed.load() ? "Not changed. Windows asks for administrator permission."
                                                         : "Windows asks for administrator permission.");
        break;
      default:
        break;
    }
    ImGui::Unindent();
  }

  // The update offer, and a note when the check failed. Drawn in the Play row (DrawButtons), not at the top of the long
  // page, so it is in view whenever Play is (B-294).
  void DrawUpdate() {
    if (!update_ || !update_->done.load()) return;
    const UpdateInfo& info = update_->info;
    if (!update_logged_) {
      update_logged_ = true;
      if (!info.error.empty()) REXLOG_INFO("setup: update check failed: {}", info.error);
      else REXLOG_INFO("setup: newest release {}, this is {}", info.latest, AppVersion());
    }
    if (!update_->failed_note.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
      ImGui::TextWrapped("%s", update_->failed_note.c_str());
      ImGui::PopStyleColor();
    }
    if (!info.error.empty()) {
      WrappedDisabled("Couldn't check for updates.");
      return;
    }
    if (info.latest.empty() || !IsNewerVersion(info.latest, AppVersion())) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
    ImGui::TextWrapped("Skullburglar %s is out (you have %s).", info.latest.c_str(), AppVersion());
    ImGui::PopStyleColor();
    UpdateState& u = *update_;
    const bool can_update = u.has_asset && u.blocker.empty();
    ImGui::BeginDisabled(UpdateBusy());
    if (can_update) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Update now")) StartInstall();
    }
    if (!info.page.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Download it")) OpenInBrowser(info.page);
    }
    ImGui::EndDisabled();
    if (UpdateBusy()) {
      const auto stage = UpdateStage(u.progress.stage.load());
      const double done = double(u.progress.done.load()) / 1e6, total = double(u.progress.total.load()) / 1e6;
      if (stage == UpdateStage::kDownloading) {
        ImGui::TextDisabled("Downloading %.1f of %.1f MB...", done, total);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) u.progress.cancel = true;
      } else if (stage == UpdateStage::kVerifying) {
        ImGui::TextDisabled("Checking the download...");
      } else {
        ImGui::TextDisabled("Installing...");
      }
    } else if (u.finished.load()) {
      ImGui::TextDisabled("Updated. Restarting...");
      if (!quit_requested_) {
        quit_requested_ = true;
        pad_.reset();
        quit_();
        return;
      }
    } else {
      std::string error;
      {
        std::lock_guard<std::mutex> lock(u.mutex);
        error = u.install_error;
      }
      if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        ImGui::TextWrapped("The update didn't install (%s). Your install is unchanged; \"Download it\" gets the new version by hand.",
                           error.c_str());
        ImGui::PopStyleColor();
      } else if (u.needs_move) {
        WrappedDisabled("To update from here, move Skullburglar to your Applications folder first.");
      } else if (u.has_asset && !u.blocker.empty()) {
        WrappedDisabled("Can't update in place: %s.", u.blocker.c_str());
      }
    }
    ImGui::Spacing();
  }

  void DrawGameFiles() {
    ImGui::TextUnformatted("Game files");
    if (installed_ && !show_reinstall_) {
      WrappedDisabled("Installed in %s", bots::PlainPathUtf8(game_dir_).c_str());
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
    WrappedDisabled("%s", bots::PlainPathUtf8(game_dir_).c_str());
    DrawFolderChoice();
    ImGui::BeginDisabled(Extracting());
    ImGui::SetNextItemWidth(-S(110.0f));
    ImGui::InputTextWithHint("##iso", "Path to the .iso file", iso_path_, sizeof(iso_path_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
      auto picked = OpenPicker(PickIsoFile);
      if (!picked.empty()) SetIsoPath(bots::PathUtf8(picked));
    }
    if (!installed_) ImGui::SetItemDefaultFocus();
    ImGui::BeginDisabled(iso_path_[0] == 0);
    if (ImGui::Button(installed_ ? "Reinstall" : "Install", ImVec2(S(160), 0))) StartExtract();
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (Extracting()) {
      uint64_t done = progress_->bytes_done, total = progress_->bytes_total;
      float fraction = total ? float(double(done) / double(total)) : 0.0f;
      char overlay[96];
      std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f GB", done / 1e9, total / 1e9);
      ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);
      WrappedDisabled("File %u of %u: %s", progress_->files_done.load(),
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
    if (!SamePath(game_dir_, default_game_dir_)) {
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
    if (old_game_dir_.empty() || SamePath(old_game_dir_, game_dir_) || !IsGameDir(old_game_dir_)) return;
    ImGui::TextWrapped("The game files in %s aren't used any more: delete that folder to free about 7 GB.",
                       bots::PlainPathUtf8(old_game_dir_).c_str());
  }

  void UseGameDir(const std::filesystem::path& dir) {
    if (dir.empty()) {  // refused; folder_error_ says why
      if (!folder_error_.empty()) REXLOG_WARN("setup: {}", folder_error_);
      return;
    }
    folder_error_.clear();
    error_.clear();
    game_dir_ = bots::LongPath(dir);  // a folder 260+ characters deep needs the extended-length form (path_utf8.h)
    installed_ = IsGameDir(game_dir_);
    REXLOG_INFO("setup: game folder {} ({})", bots::PathUtf8(game_dir_), installed_ ? "installed" : "missing");
    if (installed_ || SamePath(game_dir_, default_game_dir_)) RememberGameDir();
  }

  // bots.toml names the folder only when it isn't the default, so a player who never chooses one keeps no entry.
  void RememberGameDir() {
    const std::filesystem::path value = SamePath(game_dir_, default_game_dir_) ? std::filesystem::path() : game_dir_;
    if (value.empty() && !ConfigHasPath(config_path_, "game_data_root")) return;  // nothing to change
    std::string err = SetConfigPath(config_path_, "game_data_root", value);
    if (!err.empty()) REXLOG_WARN("setup: {}", err);
  }

  void DrawSettings() {
    ImGui::TextUnformatted("Settings");
    WrappedDisabled("Saved in %s", bots::PlainPathUtf8(config_path_).c_str());
    if (const DataRoot& data = ActiveDataRoot(); data.source != DataSource::kDefault) {
      WrappedDisabled("Portable mode (%s): saves, settings, shader cache and logs are in %s",
                          data.source == DataSource::kEnv ? "BOTS_DATA_DIR" : "portable.txt",
                          bots::PlainPathUtf8(data.root).c_str());
    }
    DrawReset();
    for (auto& s : settings_) {
      if (s.binding) continue;  // DrawBindings
      if (s.cvar == "pre1_occlusion" && Value("renderer") != "pre1") continue;  // only the pre-1.0 renderer has it
      if (!s.section.empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText(s.section.c_str());
      }
      // The rest of the keyboard and mouse section only matters with it on.
      const bool kbm_off = s.cvar.starts_with("kbm_") && Value("kbm") == "false";
      // Anti-aliasing and sharpening are n1 passes: greyed out with the pre-1.0 renderer, like HDR.
      const bool n1_only = (s.cvar == "anti_aliasing" || s.cvar == "sharpening") && Value("renderer") == "pre1";
      ImGui::BeginDisabled(Extracting() || kbm_off || n1_only);
      DrawSetting(s, S(260.0f));
      ImGui::EndDisabled();
      if (s.cvar == "vulkan_hdr" && Value("renderer") == "pre1")
        WrappedDisabled("HDR requires the new renderer.");
      if (s.cvar == "sharpening" && Value("renderer") == "pre1")
        WrappedDisabled("Anti-aliasing and sharpening require the new renderer.");
      if (s.cvar == "user_signin_live" && s.value == "true") DrawFirewall();
      if (s.cvar == "diagnostic_logging") {
        ImGui::Indent();
        WrappedDisabled("Logs are in %s", bots::PlainPathUtf8(LogFolder()).c_str());
        ImGui::Unindent();
      }
    }
    DrawBindings();
  }

  // Portable mode (B-386): saves or bots.toml sitting in the data folder instead of its user/ folder. Say where they
  // belong and offer to move them, after a confirmation that says what happens. Never moves anything by itself.
  void DrawMisplaced() {
    if (misplaced_.empty() && misplaced_note_.empty()) return;
    const ImVec4 warn(1.0f, 0.75f, 0.3f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, warn);
    if (!misplaced_.empty()) ImGui::TextWrapped("%s", MisplacedMessage(ActiveDataRoot(), misplaced_).c_str());
    else ImGui::TextWrapped("%s", misplaced_note_.c_str());
    ImGui::PopStyleColor();
    if (!misplaced_.empty()) {
      ImGui::BeginDisabled(Extracting());
      if (ImGui::Button("Move them to the user folder...")) ImGui::OpenPopup("Move your saves and settings?");
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Not now")) misplaced_.clear();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(520.0f), 0.0f));
    if (!ImGui::BeginPopupModal("Move your saves and settings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::string list;
    for (const auto& n : misplaced_) list += (list.empty() ? "" : ", ") + n;
    ImGui::TextWrapped("This moves %s from %s into %s, where the game looks for them. Nothing is copied or deleted, and "
                       "nothing already in the user folder is replaced.",
                       list.c_str(), bots::PlainPathUtf8(ActiveDataRoot().root).c_str(),
                       bots::PlainPathUtf8(LayoutFor(ActiveDataRoot().root).user).c_str());
    ImGui::Spacing();
    if (ImGui::Button("Move them", ImVec2(S(160), 0))) {
      std::string err;
      const auto moved = MoveMisplacedGameData(ActiveDataRoot(), misplaced_, err);
      REXLOG_INFO("setup: moved {} of {} entries from the data folder into user/{}{}", moved.size(), misplaced_.size(),
                  err.empty() ? "" : ": ", err);
      const bool toml_moved = std::find(moved.begin(), moved.end(), "bots.toml") != moved.end();
      if (toml_moved) settings_file_moved_ = true;
      misplaced_note_ = err;
      if (!moved.empty()) {
        misplaced_note_ += (err.empty() ? "" : " ") + std::string("Moved. Your saves are used from now on.");
        if (toml_moved)
          misplaced_note_ += " Your settings file moved too: it takes effect the next time you start Skullburglar "
                             "(this session uses the defaults, and changes made now aren't saved).";
      }
      misplaced_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // "Reset to defaults" with a confirmation that says what it does and doesn't touch (B-359).
  void DrawReset() {
    ImGui::BeginDisabled(Extracting());
    if (ImGui::SmallButton("Reset to defaults...")) ImGui::OpenPopup("Reset to defaults?");
    ImGui::EndDisabled();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(520.0f), 0.0f));
    if (!ImGui::BeginPopupModal("Reset to defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    int differ = 0;
    for (const auto& s : settings_)
      if (!s.locked && !ResetKeeps(s.cvar) && !SameValue(s.value, s.default_value)) ++differ;
    ImGui::TextWrapped("This puts every setting in this window back to the defaults of version %s: the display and "
                       "graphics options, the controller and co-op options, and the keyboard and mouse options "
                       "including every key binding.",
                       AppVersion());
    ImGui::Spacing();
    ImGui::TextWrapped("It does not change the game files folder or where your data is kept, \"Check for new versions\", "
                       "or anything a launch option has set. Your saves are not touched.");
    ImGui::Spacing();
    ImGui::TextWrapped("The reset keys are removed from the settings file, so a later version's new defaults apply to "
                       "you. %d of the settings here differ from the defaults now.",
                       differ);
    ImGui::Spacing();
    if (ImGui::Button("Reset settings", ImVec2(S(160), 0))) {
      std::string err = ResetSettings(settings_, config_path_, nullptr);
      if (!err.empty()) error_ = err;
      capture_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // Where this run's log goes: next to --log_file when one is set (the Linux AppImage's launcher script and the macOS
  // app set it: ~/.local/state/bots-recomp/logs, ~/Library/Logs/Skullburglar), else the logs folder next to the program.
  static std::filesystem::path LogFolder() {
    const std::string file = rex::cvar::Query<std::string>("log_file");
    if (!file.empty()) return bots::PathFromUtf8(file).parent_path();
    return rex::filesystem::GetExecutableFolder() / "logs";
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
                            ImVec2(0, S(240)))) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed, S(150.0f));
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
    DrawUpdate();
    const bool pad = pad_ && pad_->Connected();
    const bool ps = pad && pad_->PlayStation();
    char play[48];
    std::snprintf(play, sizeof(play), "Play  (%s)##play", pad ? (ps ? "Options" : "Start") : "Enter");
    const bool can_play = installed_ && !Extracting() && !UpdateBusy();
    // The way in, so it stands out from the other buttons.
    const ImVec4* colors = ImGui::GetStyle().Colors;
    ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_ButtonHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_ButtonActive]);
    ImGui::BeginDisabled(!can_play);
    const bool pressed = ImGui::Button(play, ImVec2(S(220), ImGui::GetFrameHeight() * 1.5f));
    if (can_play) ImGui::SetItemDefaultFocus();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (pressed || QuickPlayPressed()) {
      Play();
      return;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(UpdateCommitted());
    const bool quit_pressed = ImGui::Button("Quit", ImVec2(S(120), ImGui::GetFrameHeight() * 1.5f));
    ImGui::EndDisabled();
    if (quit_pressed) {
      if (Extracting()) progress_->cancel = true;
      if (UpdateBusy()) update_->progress.cancel = true;
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
    if (!SamePath(game_dir_, DefaultGameDir()) && !IsGameDir(game_dir_) && std::filesystem::exists(game_dir_, ec) &&
        !(std::filesystem::is_directory(game_dir_, ec) && std::filesystem::is_empty(game_dir_, ec))) {
      error_ = bots::PlainPathUtf8(game_dir_) + " already exists and doesn't hold the game's files. Choose another folder.";
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
    std::string err = settings_file_moved_ ? std::string() : SaveSettings(settings_, config_path_);
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
  float ui_scale_ = 1.0f;  // ui_scale.h; set at the start of every frame
  float logged_width_ = 0, logged_height_ = 0, logged_scale_ = 0, logged_font_ = 0, logged_overflow_ = -1;
  float settings_overflow_ = 0;  // ImGui::GetScrollMaxX() of the settings list (the window's is in its storage)
  bool installed_ = false;
  bool quit_requested_ = false;  // an installed update asked the program to quit
  bool show_reinstall_ = false;
  std::vector<std::string> misplaced_;  // game data in the portable data folder that belongs in user/ (portable.h)
  std::string misplaced_note_;          // what moving them did, shown in place of the offer
  bool settings_file_moved_ = false;    // bots.toml was moved in: saving this window's settings would overwrite it
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

// F6 display settings. Changes apply on the next frame and are saved when the window closes.
class DisplayDialog : public rex::ui::ImGuiDialog {
 public:
  DisplayDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path, std::function<const char*()> status,
                std::function<void()> closed)
      : ImGuiDialog(drawer), config_path_(std::move(config_path)), status_(std::move(status)),
        closed_(std::move(closed)), settings_(LoadSettings()) {
    std::erase_if(settings_, [](const Setting& s) {
      return !s.cvar.starts_with("vulkan_hdr") && s.cvar != "fov_offset";
    });
    original_settings_ = settings_;
  }

  void RequestClose() { close_ = true; }

 protected:
  void OnClose() override {
    std::erase_if(settings_, [this](const Setting& s) {
      return std::any_of(original_settings_.begin(), original_settings_.end(), [&](const Setting& original) {
        return original.cvar == s.cvar && (original.value == s.value ||
            (s.slider_max > 0 && std::strtod(original.value.c_str(), nullptr) == std::strtod(s.value.c_str(), nullptr)));
      });
    });
    if (!settings_.empty()) SaveSettings(settings_, config_path_);
    closed_();
  }

  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowSize(ImVec2(600, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    bool open = !close_;
    if (ImGui::Begin("Display settings (F6)", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted(status_());
      ImGui::TextDisabled("Enabling HDR requires a restart.");
      ImGui::Separator();
      for (auto& s : settings_) {
        if (DrawSetting(s)) rex::cvar::SetFlagByName(s.cvar, s.value);
      }
      if (ImGui::Button("Reset brightness and grading")) {
        for (auto& s : settings_) {
          if (s.locked || s.cvar == "vulkan_hdr" || s.cvar == "fov_offset") continue;
          s.value = s.default_value;
          rex::cvar::SetFlagByName(s.cvar, s.value);
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Close")) open = false;
    }
    ImGui::End();
    if (!open) Close();
  }

 private:
  std::filesystem::path config_path_;
  std::function<const char*()> status_;
  std::function<void()> closed_;
  std::vector<Setting> settings_;
  std::vector<Setting> original_settings_;
  bool close_ = false;
};

}  // namespace bots::setup
