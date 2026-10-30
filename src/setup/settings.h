// Player-facing settings shown by the setup window (B-061) and later the in-game overlay (B-050).
// Each one is an SDK cvar, so the usual precedence holds: bots.toml < REX_* env < --flag. The
// window edits values coming from the defaults or bots.toml. Values forced by env or a flag are
// shown locked. Saving writes the keys to bots.toml (other keys in the file are kept) and applies
// them as runtime values for this launch.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bots::setup {

struct Choice {
  std::string value;  // cvar value as text
  std::string label;
};

struct Setting {
  std::string cvar;               // also the bots.toml key
  std::string also_cvar;          // second cvar kept equal (resolution scale y)
  std::string label;
  std::string help;
  std::vector<Choice> choices;    // empty = checkbox (bool cvar), unless slider_max is set
  double slider_min = 0, slider_max = 0;  // slider_max > 0: a slider for a double cvar
  bool slider_linear = false;     // linear instead of the default logarithmic slider
  std::string slider_format = "%.2f";  // printf format of the slider's value
  std::string section;            // starts a new heading in the window
  bool binding = false;           // a key binding (kbm_*: comma-separated key names), shown in the bindings table
  std::string default_value;      // the cvar's built-in value (Reset)
  // Live state for the window.
  std::string value;              // current value as text
  bool locked = false;            // set by env or command line: shown, not editable
};

// The curated list, with current values read from the cvar registry.
std::vector<Setting> LoadSettings();

// Applies every unlocked setting as a runtime cvar value and writes them to `config_path`.
// Returns an error message, empty on success.
std::string SaveSettings(const std::vector<Setting>& settings,
                         const std::filesystem::path& config_path);

// Marks a cvar whose REX_* env value is the app's own built-in default (BotsApp::SetCvarDefault),
// not a launch option, so the window still lets the player change it.
void MarkAppDefault(const std::string& cvar);

// True when bots.toml sets `key` (then the app's built-in env defaults must not override it).
bool ConfigHasKey(const std::filesystem::path& config_path, const std::string& key);

// A string value from bots.toml, empty if missing (game_data_root is read before the SDK loads
// the file).
std::string ConfigString(const std::filesystem::path& config_path, const std::string& key);

// Sets one string key in bots.toml, or removes it when `value` is empty, keeping the rest of the file. Returns an
// error message, empty on success.
std::string SetConfigString(const std::filesystem::path& config_path, const std::string& key,
                            const std::string& value);

// A folder in bots.toml. Text that is valid UTF-8 is stored under `key` as it is. A Linux folder name can hold bytes
// that aren't UTF-8 (a Latin-1 name from an old SD card), and toml++ would silently change them on save, so such a
// path goes under `<key>_hex` as the hex of its bytes instead. ConfigPath reads either; SetConfigPath removes both
// when `path` is empty.
std::filesystem::path ConfigPath(const std::filesystem::path& config_path, const std::string& key);
bool ConfigHasPath(const std::filesystem::path& config_path, const std::string& key);
std::string SetConfigPath(const std::filesystem::path& config_path, const std::string& key,
                          const std::filesystem::path& path);

}  // namespace bots::setup
