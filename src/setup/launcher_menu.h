// The launcher's full-screen menu (B-387, GitHub #50): a game-style front page (Play, the settings sections, Quit, with
// status cards for the game files, updates and renderer) where each section opens a full-screen settings page. Mouse
// and keyboard and controllers work alike: every action has a key, a button and a click, and the prompts at the
// bottom show the glyphs of whatever was used last.
//
// Input:
//   mouse      hover selects, click activates, wheel scrolls, drag a slider; the prompts at the bottom are buttons too
//   keyboard   arrows / Tab move, Enter or Space activates, Left / Right change a value (Shift: bigger steps), Esc goes
//              back, Q / E (or Page Up / Page Down) switch sections, Delete restores a setting's default, U updates, Ctrl+Enter
//              plays from anywhere
//   controller d-pad or left stick move, A activates, B goes back, Left / Right change a value, LT / RT bigger
//              slider steps, LB / RB switch sections, Y restores the default, X adds a key binding, Start plays
//
// The menu is drawing and input only. It edits the Setting values in place; everything else (installing, updating,
// saving, playing) it asks the window for by returning a LauncherAction. It needs ImGui and nothing from the SDK, so
// tools/launcher_preview.cpp can draw it without the game.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_logic.h"
#include "launcher_style.h"
#include "settings.h"

namespace bots::setup {

enum class LauncherAction {
  kNone,
  kPlay,
  kQuit,
  kBrowseIso,       // open the file picker for the disc image
  kInstall,         // install (or reinstall) from the disc image path
  kCancelInstall,
  kShowReinstall,   // the installed game files page offers a reinstall
  kHideReinstall,
  kChooseFolder,    // pick another folder for the game files
  kUseDefaultFolder,
  kUpdateNow,
  kCancelUpdate,
  kDownloadPage,
  kFixFirewall,
  kResetAll,        // Reset to defaults, confirmed
  kMoveSaves,       // portable mode: move the misplaced saves and settings into user/, confirmed (B-386)
  kDismissSaves,    // "Not now" on the misplaced-saves notice
  kOpenLogFolder,   // About & logs: show the logs folder in the system's file browser
  // Portable mode keeps the game files in its own folder (B-400)
  kCopyFromShared,  // Game files: copy the shared install into the portable folder
  kUseSharedInPlace,  // Game files: keep playing from the shared folder
  kOfferCopy,       // the copy offer: Copy the game files here
  kOfferDisc,       // the copy offer: Install from a disc image instead
  kOfferNotNow,     // the copy offer: Not now
};

// What the window knows that the menu shows. Filled every frame.
struct LauncherStatus {
  std::string version;
  float dpi_scale = 1.0f;  // screen pixels per logical pixel (the display scaling)
  PadKind pad = PadKind::kNone;

  // Game files
  bool installed = false;
  bool show_reinstall = false;
  bool extracting = false;
  float extract_fraction = 0.0f;
  std::string extract_progress;  // "1.2 / 6.8 GB"
  std::string extract_file;      // "File 3 of 737: ..."
  std::string game_dir;
  bool game_dir_locked = false;  // set by a launch option
  bool folder_choice = true;     // desktop builds can choose another folder
  bool can_use_default = false;  // the chosen folder isn't the default one
  std::string unused_copy;       // an installed copy that isn't used any more
  std::string install_error;
  bool install_error_is_info = false;  // "Cancelled.": drawn plain, not as an error
  std::string folder_error;
  // Portable mode (B-400): the game files live in the portable folder; the shared install can be copied into it.
  bool copying = false;                 // the running install is the copy from the shared folder
  std::string portable_game_note;       // where the portable game files are kept (or the shared folder isn't)
  std::string shared_dir;               // the shared install (the copy's source)
  bool can_copy_from_shared = false;    // this folder holds no game and the shared one does
  std::string shared_note;              // "Using the shared game folder..." (an existing portable user chose Not now)
  bool offer_open = false;              // the copy offer is up
  std::string offer_dest, offer_size_text, offer_free_text, offer_error;  // size and free space, or why they're unknown
  bool offer_enough = true;             // the free space holds the copy
  bool offer_prior_use = false;         // this portable folder was used before: Not now keeps the shared folder
  char* iso_path = nullptr;  // the disc image path text box
  size_t iso_path_size = 0;

  // Auto-save: every change is written to bots.toml as it is made
  bool saved_flash = false;     // a write just succeeded: "Saved" in the footer for a moment
  std::string save_failed;      // the last write failed: why, short (the changes stay in memory for this session)
  std::string save_failed_full;

  // Settings file, data and logs
  std::string config_path;
  std::string portable_note;  // portable mode: where everything is kept
  std::string log_folder;
  std::string reset_note;     // after Reset to defaults
  // Portable-mode notices (B-386, B-389). The window checks the data folder; the menu shows them on About & logs, with a
  // card and a badge on the main menu.
  std::string saves_warning;     // saves or bots.toml sit in the data folder instead of user/: where they belong
  bool saves_can_move = false;   // offer "Move them to the user folder"
  std::string saves_move_text;   // what moving does (the confirmation's body)
  std::string saves_note;        // the misnamed portable.txt hint, or what moving them did
  bool saves_note_is_error = true;
  std::string save_error;

  // Updates
  bool update_available_feature = false;  // a release build with an update URL
  bool update_checking = false;
  bool update_check_failed = false;
  bool update_newer = false;
  std::string update_latest;
  std::string update_failed_note;  // the last update's setup program never finished (Windows)
  std::string update_note;         // why there's no Update now, or what went wrong
  bool update_note_is_error = false;
  bool update_can_install = false;
  bool update_can_download = false;
  bool update_busy = false;
  bool update_committed = false;  // past the download: can't be cancelled
  bool update_finished = false;
  int update_stage = 0;  // 0 downloading, 1 verifying, 2+ installing (UpdateStage minus kDownloading)
  double update_done_mb = 0, update_total_mb = 0;

  // Windows Firewall for LAN co-op (FirewallState as int)
  int firewall = 0;
  bool firewall_fixing = false;
  bool firewall_fix_failed = false;
};

// What a layout pass saw (BOTS_SETUP_SHOT and the preview tool check these at every resolution).
struct LauncherLayoutReport {
  int truncated = 0;         // text that didn't fit its box and was cut with "..."
  int prompts_dropped = 0;   // footer prompts left out for lack of room
  float min_text_px = 1e9f;  // the smallest text drawn, in screen pixels
  bool focus_visible = true; // the focused row is fully in view
  int rows = 0;              // focusable rows on the page
  std::vector<std::string> notes;
  void Reset() { *this = LauncherLayoutReport(); }
};

class LauncherMenu {
 public:
  LauncherMenu();

  // Draws one frame over the whole display and handles its input. `settings` are edited in place.
  LauncherAction Draw(std::vector<Setting>& settings, const LauncherStatus& status, const LauncherFonts& fonts);

  // A key binding is waiting for its key, or a confirmation is open: the window's own shortcuts stay off.
  bool Busy() const { return capture_ >= 0 || confirm_reset_ || confirm_move_ || offer_open_; }

  // A slider is held by the mouse (auto-save waits for the release).
  bool Dragging() const { return dragging_row_ >= 0; }

  LauncherPage page() const { return page_; }
  LastInput last_input() const { return last_input_; }
  const LauncherLayoutReport& report() const { return report_; }

  // Test hooks (BOTS_SETUP_SCRIPT, the preview tool).
  void GoTo(LauncherPage page);
  bool FocusSetting(const std::string& cvar);  // opens its page and selects it
  void FocusRow(int row);
  void SetLastInput(LastInput input) { last_input_ = input; forced_input_ = true; }
  void ShowGlyphStrip(bool on) { glyph_strip_ = on; }  // Start and Back drawn in a corner, for previews
  void OpenResetConfirm() { confirm_reset_ = true; confirm_focus_ = 1; }
  void OpenMoveConfirm() { confirm_move_ = true; confirm_focus_ = 1; }

 private:
  struct Intent;
  struct RowState;

  // Pages
  void DrawMain(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in);
  void DrawPage(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in);
  void DrawPageRows(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in, float width);
  void DrawGameFilesRows(const LauncherStatus& st, Intent& in, float width);
  void DrawUpdatesRows(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in, float width);
  void DrawAboutRows(std::vector<Setting>& settings, const LauncherStatus& st, Intent& in, float width);
  void DrawBindingRows(std::vector<Setting>& settings, Intent& in, float width);
  void DrawHelp(const ImVec2& min, const ImVec2& max);
  void DrawBackground(ImDrawList* dl, const ImVec2& size, bool page);
  void DrawStatusCards(const std::vector<Setting>& settings, const LauncherStatus& st, ImVec2 pos, float width,
                       float max_y);
  void DrawFooter(const LauncherStatus& st, Intent& in, float top);
  void DrawConfirm(const LauncherStatus& st, Intent& in);  // Reset to defaults, or Move the saves
  void DrawCapture(std::vector<Setting>& settings, Intent& in);
  void DrawTabs(ImVec2 pos, float width, Intent& in);

  // Rows
  RowState BeginRow(const char* id, float height, float width);
  void EndRow(const RowState& row);
  void SettingRow(Setting& s, const std::vector<Setting>& all, Intent& in, float width, bool disabled,
                  const char* disabled_note);
  bool ActionRow(const char* id, const std::string& label, const std::string& value, const std::string& help,
                 Intent& in, float width, bool enabled = true, bool primary = false);
  void IsoPathRow(const LauncherStatus& st, Intent& in, float width);
  void Note(const std::string& text, ImU32 color, float width, FontRole role = FontRole::kBody);
  void GroupHeading(const char* text, float width);
  void PageGroupHeading(const Setting& s, std::string& group, float width);
  void ProgressRow(float fraction, const std::string& overlay, float width);

  // Drawing helpers
  float S(float px) const { return px * scale_; }
  ImFont* Font(FontRole role) const;
  float Size(FontRole role) const { return kRoleSize[int(role)] * scale_; }
  ImVec2 Measure(FontRole role, const std::string& text, float wrap = 0.0f) const;
  void Text(ImDrawList* dl, FontRole role, ImVec2 pos, ImU32 col, const std::string& text, float max_width = 0.0f);
  float TextWrapped(ImDrawList* dl, FontRole role, ImVec2 pos, ImU32 col, const std::string& text, float width);
  float Glyph(ImDrawList* dl, ImVec2 pos, float height, int button, const char* key) const;
  float GlyphWidth(float height, int button, const char* key) const;
  void Toggle(ImDrawList* dl, ImVec2 min, ImVec2 max, bool on, bool focused, bool enabled);
  void CountText(FontRole role);

  // State
  LauncherPage page_ = LauncherPage::kMain;
  int main_focus_ = 0;
  int focus_[16] = {};
  int focus_target_ = -1;       // set a focus once the page's rows exist (FocusSetting)
  std::string focus_cvar_;
  bool scroll_to_focus_ = true;
  bool started_ = false;        // the first frame chose where focus starts
  bool was_extracting_ = false;  // an install was running last frame: when it ends well, Play is the next step
  int row_index_ = 0;           // rows drawn so far this frame
  int prev_rows_ = 0;           // rows on the page last frame
  int dragging_row_ = -1;       // a slider held by the mouse
  int capture_ = -1;            // settings index of the binding waiting for a key
  bool capture_append_ = false;
  int capture_armed_frames_ = 0;
  float capture_escape_held_ = -1.0f;
  bool confirm_reset_ = false;
  bool confirm_move_ = false;   // the move-saves confirmation
  bool offer_open_ = false;     // the portable copy offer (three buttons), from the status
  int confirm_focus_ = 1;       // 0 reset, 1 cancel
  bool text_input_active_ = false;
  bool focus_text_input_ = false;
  LastInput last_input_ = LastInput::kKeyboardMouse;
  bool forced_input_ = false;
  bool glyph_strip_ = false;
  PromptDevice device_ = PromptDevice::kKeyboardMouse;
  ImVec2 last_mouse_{-1e9f, -1e9f};
  float scale_ = 1.0f;
  float pixel_scale_ = 1.0f;
  const LauncherFonts* fonts_ = nullptr;
  LauncherAction action_ = LauncherAction::kNone;
  LauncherLayoutReport report_;

  // The focused row's help (filled while the rows draw, shown in the help panel).
  std::string help_title_, help_text_, help_default_, help_note_;
  bool help_locked_ = false;
};

}  // namespace bots::setup
