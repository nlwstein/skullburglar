// Setup / launcher window (B-061). Shown before the game starts on every launch (BotsApp::
// OnFinalizePaths has the exceptions). Without game files, the player points it at their own disc
// image once and the files are copied to a per-user folder (app_paths.h), or on the desktop builds to a folder the
// player chooses, kept in bots.toml as game_data_root. The window also edits the player settings (settings.h). Play
// saves them and starts the game.
// Test hooks: BOTS_SETUP_ISO=<path> prefills the image path; BOTS_SETUP_FOLDER=<path> chooses that folder for the
// game files as "Choose another folder..." would (desktop builds); BOTS_SETUP_AUTO=1 installs (when a path is set and
// nothing is installed) and presses Play, so the flow runs unattended.
// Release builds also check for a newer version when the window opens (update_check.h).
// It draws the full-screen menu (launcher_menu.h, B-387): mouse, keyboard and controllers alike (pad_nav.h feeds the
// controllers in). The menu edits the settings; this class does everything else and saves on Play.
// BOTS_SETUP_SHOT=<dir> writes PNG screenshots of the launcher at any size, driven by BOTS_SETUP_SCRIPT (test-only;
// launcher_shot.h).
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
#include "launcher_menu.h"
#include "launcher_shot.h"
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
    PollWorker();
    if (auto_) RunAuto();
    if (!shots_done_) RunShots(io);
    // BOTS_SETUP_SHOT_QUIT: close a few frames after the shots, as the Quit entry would.
    if (shot_quit_frames_ > 0 && --shot_quit_frames_ == 0 && !quit_requested_) {
      quit_requested_ = true;
      pad_.reset();
      quit_();
      return;
    }
    // An installed update restarts the program.
    if (update_ && update_->finished.load() && !quit_requested_) {
      quit_requested_ = true;
      pad_.reset();
      quit_();
      return;
    }
    // Windows asks about the network as the launcher opens with LAN co-op on, not in the middle of a story.
    if (Value("user_signin_live") == "true" && FirewallState(firewall_->state.load()) == FirewallState::kNotAsked &&
        !firewall_asked_) {
      firewall_asked_ = true;
      firewall_->asked = true;
      AskFirewall();
    }
    LogUpdate();
    const LauncherStatus status = Status();
    const LauncherAction action = menu_.Draw(settings_, status, LaunchFonts());
    LogUiScale(io, status.dpi_scale);
    Do(action);  // last: Play and Quit can delete this dialog
  }

 private:
  // Screen pixels per logical pixel (Windows display scaling; 1 on macOS, where the window's size is in points).
  float DpiScale() const {
    const rex::ui::Window* w = imgui_drawer()->window();
    return w ? float(w->GetDpi()) / float(w->GetMediumDpi()) : 1.0f;
  }

  // One log line per change of size or scale: the numbers a "text too small" report needs.
  void LogUiScale(const ImGuiIO& io, float dpi) {
    const float scale = LauncherMenuScale(io.DisplaySize.x, io.DisplaySize.y, std::getenv("BOTS_UI_SCALE"));
    if (io.DisplaySize.x == logged_width_ && io.DisplaySize.y == logged_height_ && scale == logged_scale_) return;
    logged_width_ = io.DisplaySize.x, logged_height_ = io.DisplaySize.y, logged_scale_ = scale;
    const LauncherLayoutReport& r = menu_.report();
    REXLOG_INFO("setup: menu scale {:.2f} (window {:.0f}x{:.0f} logical, display scaling {:.0f}%, body text about "
                "{:.1f} px on screen, font bake {:.2f}x the size drawn); layout: {} cut-off texts, {} prompts left out",
                scale, io.DisplaySize.x, io.DisplaySize.y, dpi * 100.0f, double(r.min_text_px),
                double(LaunchFonts().Mismatch(scale * dpi)), r.truncated, r.prompts_dropped);
  }

  void LogUpdate() {
    if (update_logged_ || !update_ || !update_->done.load()) return;
    update_logged_ = true;
    const UpdateInfo& info = update_->info;
    if (!info.error.empty()) REXLOG_INFO("setup: update check failed: {}", info.error);
    else REXLOG_INFO("setup: newest release {}, this is {}", info.latest, AppVersion());
  }

  // What the menu shows, from the window's own state.
  LauncherStatus Status() {
    LauncherStatus st;
    st.version = AppVersion();
    st.dpi_scale = DpiScale();
    st.pad = pad_ ? pad_->Kind() : PadKind::kNone;
    st.installed = installed_;
    st.show_reinstall = show_reinstall_;
    st.extracting = Extracting();
    if (st.extracting) {
      const uint64_t done = progress_->bytes_done, total = progress_->bytes_total;
      st.extract_fraction = total ? float(double(done) / double(total)) : 0.0f;
      char text[96];
      std::snprintf(text, sizeof(text), "%.1f / %.1f GB", done / 1e9, total / 1e9);
      st.extract_progress = text;
      std::snprintf(text, sizeof(text), "File %u of %u: ", progress_->files_done.load(), progress_->files_total.load());
      st.extract_file = text + progress_->Current();
    }
    st.game_dir = bots::PlainPathUtf8(game_dir_);
    st.game_dir_locked = game_dir_locked_;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    st.folder_choice = false;
#endif
    st.can_use_default = !SamePath(game_dir_, default_game_dir_);
    if (!old_game_dir_.empty() && !SamePath(old_game_dir_, game_dir_) && IsGameDir(old_game_dir_))
      st.unused_copy = bots::PlainPathUtf8(old_game_dir_);
    st.install_error = error_;
    st.folder_error = folder_error_;
    st.iso_path = iso_path_;
    st.iso_path_size = sizeof(iso_path_);
    st.config_path = bots::PlainPathUtf8(config_path_);
    if (const DataRoot& data = ActiveDataRoot(); data.source != DataSource::kDefault) {
      st.portable_note = std::string("Portable mode (") +
                         (data.source == DataSource::kEnv ? "BOTS_DATA_DIR" : "portable.txt") +
                         "): saves, settings, shader cache and logs are in " + bots::PlainPathUtf8(data.root);
    }
    st.log_folder = bots::PlainPathUtf8(LogFolder());
    st.reset_note = reset_note_;
    // Portable mode (B-386, B-389): saves or bots.toml in the data folder instead of its user/ folder, or a portable.txt
    // that Windows named portable.txt.txt. Never moves anything by itself.
    if (!misplaced_.empty()) {
      st.saves_warning = MisplacedMessage(ActiveDataRoot(), misplaced_);
      st.saves_can_move = true;
      std::string list;
      for (const auto& n : misplaced_) list += (list.empty() ? "" : ", ") + n;
      st.saves_move_text = "This moves " + list + " from " + bots::PlainPathUtf8(ActiveDataRoot().root) + " into " +
                           bots::PlainPathUtf8(LayoutFor(ActiveDataRoot().root).user) +
                           ", where the game looks for them. Nothing is copied or deleted, and nothing already in the "
                           "user folder is replaced.";
    }
    st.saves_note = misplaced_note_;
    st.saves_note_is_error = misplaced_note_is_error_;
    st.update_available_feature = UpdateCheckAvailable();
    if (update_) {
      UpdateState& u = *update_;
      st.update_checking = !u.done.load();
      if (u.done.load()) {
        const UpdateInfo& info = u.info;
        st.update_check_failed = !info.error.empty();
        st.update_failed_note = u.failed_note;
        st.update_newer = info.error.empty() && !info.latest.empty() && IsNewerVersion(info.latest, AppVersion());
        st.update_latest = info.latest;
        st.update_can_install = st.update_newer && u.has_asset && u.blocker.empty();
        st.update_can_download = st.update_newer && !info.page.empty();
        st.update_busy = UpdateBusy();
        st.update_committed = UpdateCommitted();
        st.update_finished = u.finished.load();
        st.update_stage = u.progress.stage.load();
        st.update_done_mb = double(u.progress.done.load()) / 1e6;
        st.update_total_mb = double(u.progress.total.load()) / 1e6;
        std::string error;
        {
          std::lock_guard<std::mutex> lock(u.mutex);
          error = u.install_error;
        }
        if (!error.empty()) {
          st.update_note = "The update didn't install (" + error +
                           "). Your install is unchanged; the download page gets the new version by hand.";
          st.update_note_is_error = true;
        } else if (st.update_newer && u.needs_move) {
          st.update_note = "To update from here, move Skullburglar to your Applications folder first.";
        } else if (st.update_newer && u.has_asset && !u.blocker.empty()) {
          st.update_note = "Can't update in place: " + u.blocker + ".";
        }
      }
    }
    st.firewall = int(firewall_->state.load());
    st.firewall_fixing = firewall_->fixing.load() || firewall_->fix.load();
    st.firewall_fix_failed = firewall_->fix_failed.load();
    return st;
  }

  // What the menu asked for. Play and Quit delete this dialog: nothing may follow them.
  void Do(LauncherAction action) {
    switch (action) {
      case LauncherAction::kNone: return;
      case LauncherAction::kPlay:
        if (installed_ && !Extracting() && !UpdateBusy()) Play();
        return;
      case LauncherAction::kQuit:
        if (UpdateCommitted()) return;  // past the download the swap can't be interrupted
        if (Extracting()) progress_->cancel = true;
        if (UpdateBusy()) update_->progress.cancel = true;
        pad_.reset();
        quit_();
        return;
      case LauncherAction::kBrowseIso:
        if (!Extracting()) {
          auto picked = OpenPicker(PickIsoFile);
          if (!picked.empty()) SetIsoPath(bots::PathUtf8(picked));
        }
        return;
      case LauncherAction::kInstall:
        if (!Extracting()) StartExtract();
        return;
      case LauncherAction::kCancelInstall:
        if (Extracting()) progress_->cancel = true;
        return;
      case LauncherAction::kShowReinstall: show_reinstall_ = true; return;
      case LauncherAction::kHideReinstall: show_reinstall_ = false; return;
      case LauncherAction::kChooseFolder:
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
        if (!Extracting() && !game_dir_locked_) {
          const std::filesystem::path chosen = OpenPicker(PickFolder);
          if (!chosen.empty()) UseGameDir(ChosenGameDir(chosen, folder_error_));
        }
#endif
        return;
      case LauncherAction::kUseDefaultFolder:
        if (!Extracting() && !game_dir_locked_) UseGameDir(default_game_dir_);
        return;
      case LauncherAction::kUpdateNow:
        if (update_ && update_->has_asset && update_->blocker.empty() && !UpdateBusy()) StartInstall();
        return;
      case LauncherAction::kCancelUpdate:
        if (UpdateBusy()) update_->progress.cancel = true;
        return;
      case LauncherAction::kDownloadPage:
        if (update_ && update_->done.load() && !update_->info.page.empty()) OpenInBrowser(update_->info.page);
        return;
      case LauncherAction::kFixFirewall: firewall_->fix = true; return;
      case LauncherAction::kDismissSaves: misplaced_.clear(); return;
      case LauncherAction::kMoveSaves: {
        if (misplaced_.empty() || Extracting()) return;
        std::string err;
        const auto moved = MoveMisplacedGameData(ActiveDataRoot(), misplaced_, err);
        REXLOG_INFO("setup: moved {} of {} entries from the data folder into user/{}{}", moved.size(), misplaced_.size(),
                    err.empty() ? "" : ": ", err);
        const bool toml_moved = std::find(moved.begin(), moved.end(), "bots.toml") != moved.end();
        if (toml_moved) settings_file_moved_ = true;
        misplaced_note_ = err;
        misplaced_note_is_error_ = !err.empty();
        if (!moved.empty()) {
          misplaced_note_ += (err.empty() ? "" : " ") + std::string("Moved. Your saves are used from now on.");
          if (toml_moved)
            misplaced_note_ += " Your settings file moved too: it takes effect the next time you start Skullburglar "
                               "(this session uses the defaults, and changes made now aren't saved).";
        }
        misplaced_.clear();
        return;
      }
      case LauncherAction::kResetAll: {
        int changed = 0;
        const std::string err = ResetSettings(settings_, config_path_, &changed);
        if (!err.empty()) error_ = err;
        reset_note_ = err.empty() ? std::to_string(changed) + " settings were put back to this version's defaults."
                                  : "Reset failed: " + err;
        return;
      }
    }
  }

  // BOTS_SETUP_SHOT (launcher_shot.h): the launcher draws itself into PNGs at the sizes asked for, driven by
  // BOTS_SETUP_SCRIPT, on a copy of the settings. Once, on the first frame.
  void RunShots(const ImGuiIO& io) {
    shots_done_ = true;
    const std::filesystem::path dir = bots::EnvPath("BOTS_SETUP_SHOT");
    if (dir.empty()) return;
    const float dpi = DpiScale();
    std::vector<ShotSize> sizes;
    if (const char* list = std::getenv("BOTS_SETUP_SHOT_SIZES"); list && *list) sizes = ParseShotSizes(list);
    if (sizes.empty())
      sizes.push_back({int(std::lround(io.DisplaySize.x * dpi)), int(std::lround(io.DisplaySize.y * dpi)), dpi});
    const char* script_text = std::getenv("BOTS_SETUP_SCRIPT");
    const auto script = ParseShotScript(script_text && *script_text ? script_text : "shot:launcher");
    std::vector<Setting> copy = settings_;
    const LauncherStatus base = Status();
    ShotHost host;
    host.fonts = EmbeddedLauncherFontData();
    host.frame = [&](LauncherMenu& menu, const LauncherFonts& fonts, float shot_dpi, PadKind pad) {
      LauncherStatus st = base;
      st.dpi_scale = shot_dpi;
      if (pad != PadKind::kXbox || st.pad == PadKind::kNone) st.pad = pad;
      // What the menu asks for is only logged: the shots never install, update or play. Reset to defaults applies to the
      // copy, as the window's would, so a script can check it.
      const LauncherAction a = menu.Draw(copy, st, fonts);
      if (a == LauncherAction::kResetAll)
        for (auto& s : copy)
          if (!s.locked && !ResetKeeps(s.cvar)) s.value = s.default_value;
      return a;
    };
    host.value_of = [&](const std::string& cvar) {
      for (const auto& s : copy)
        if (s.cvar == cvar) return s.value;
      return std::string();
    };
    host.reset = [&] { copy = settings_; };  // each size starts from the settings as loaded
    host.log = [](const std::string& line) { REXLOG_INFO("setup: shot: {}", line); };
    const int failures = RunShotScript(host, dir, sizes, script);
    // BOTS_SETUP_SHOT_SAVE=1: keep what the script changed (at the last size) and save it as Play would, so a test can
    // check what reaches bots.toml.
    if (const char* save = std::getenv("BOTS_SETUP_SHOT_SAVE"); save && std::strcmp(save, "1") == 0) {
      settings_ = copy;
      const std::string err = SaveSettings(settings_, config_path_);
      REXLOG_INFO("setup: BOTS_SETUP_SHOT_SAVE: {}", err.empty() ? "saved" : err);
    }
    REXLOG_INFO("setup: BOTS_SETUP_SHOT: {} size(s) written to {}, {} failure(s)", sizes.size(), bots::PathUtf8(dir),
                failures);
    if (const char* q = std::getenv("BOTS_SETUP_SHOT_QUIT"); q && std::strcmp(q, "1") == 0) shot_quit_frames_ = 10;
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

  void StartExtract() {
    error_.clear();
    if (iso_path_[0] == 0) return;
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
  float logged_width_ = 0, logged_height_ = 0, logged_scale_ = 0;
  LauncherMenu menu_;
  std::string reset_note_;
  bool update_logged_ = false;
  bool shots_done_ = false;
  int shot_quit_frames_ = 0;
  bool installed_ = false;
  bool quit_requested_ = false;  // an installed update asked the program to quit
  bool show_reinstall_ = false;
  std::vector<std::string> misplaced_;  // game data in the portable data folder that belongs in user/ (portable.h)
  std::string misplaced_note_;          // what moving them did, shown in place of the offer
  bool misplaced_note_is_error_ = true;  // the misnamed-portable.txt hint is a warning; "Moved." is not
  bool settings_file_moved_ = false;    // bots.toml was moved in: saving this window's settings would overwrite it
  bool auto_ = false;
  bool auto_reinstall_ = false;
  char iso_path_[1024] = {};
  std::string error_;
  std::thread worker_;
  std::shared_ptr<ExtractProgress> progress_;
  std::shared_ptr<ExtractResult> result_;
  std::shared_ptr<std::atomic<bool>> done_;
  std::shared_ptr<UpdateState> update_;
  std::shared_ptr<FirewallWatch> firewall_;
  bool firewall_asked_ = false;
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
