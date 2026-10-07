#include "coop.h"
#include "script_input.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
extern char** environ;
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_INT32(pad, 0, "bots",
                     "Controller (1-4, connection order) that drives this instance; 0: all pads");
REXCVAR_DEFINE_BOOL(coop, false, "bots",
                    "Couch co-op: also start player 2 in a second window (see coop_pads)");
REXCVAR_DEFINE_STRING(coop_pads, "1,2", "bots", "Controllers for co-op players 1 and 2");
REXCVAR_DEFINE_BOOL(split_screen, false, "bots",
                    "One-window co-op: a second controller joins with START, the screen split in two");
REXCVAR_DEFINE_INT32(coop_player, 1, "bots", "Which co-op player this instance is (internal)");

namespace bots {
namespace {

// Player 2's identity, in either co-op mode.
constexpr uint64_t kPlayerTwoXuid = 0xB13EBABEBABE0002ull;
constexpr const char* kPlayerTwoXuidText = "B13EBABEBABE0002";
constexpr const char* kPlayerTwoGamertag = "Player2";

class PadAssignment final : public rex::input::DeviceAssignment {
 public:
  explicit PadAssignment(uint32_t ordinal) : ordinal_(ordinal) {}

  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    user0_.clear();
    bool found = false;
    for (const auto& device : devices) {
      if (device.synthetic || device.ordinal == ordinal_) user0_.push_back(device.id);
      found |= !device.synthetic && device.ordinal == ordinal_;
    }
    if (found != found_) {
      REXLOG_INFO("pad {}: {}", ordinal_ + 1, found ? "connected" : "not connected");
      found_ = found;
    }
  }

  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    if (user_index == 0) out = user0_;
  }

 private:
  uint32_t ordinal_;
  bool found_ = false;
  std::vector<rex::input::DeviceId> user0_;
};

// A default that an explicit --flag on the command line overrides.
void SetUnlessCommandLine(std::string_view name, const std::string& value) {
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kCommandLine) return;
  rex::cvar::SetFlagByName(name, value);
}

// "run/user" -> "run/user-p2"
std::filesystem::path PlayerTwoPath(std::filesystem::path p) {
  p = p.lexically_normal();
  if (!p.has_filename()) p = p.parent_path();
  std::filesystem::path out = p;
  out.replace_filename(p.stem().string() + "-p2" + p.extension().string());
  return out;
}

// Player 1's environment for player 2: no setup window, no test scripts or captures meant for
// player 1; BOTS_P2_<X>=v becomes BOTS_<X>=v (tests drive player 2 that way).
std::vector<std::string> PlayerTwoEnvironment() {
  std::vector<std::string> in;
#if defined(_WIN32)
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (wchar_t* e = block; *e; e += wcslen(e) + 1) {
      in.push_back(std::filesystem::path(e).string());
    }
    FreeEnvironmentStringsW(block);
  }
#else
  for (char** e = environ; *e; ++e) in.emplace_back(*e);
#endif
  static const std::set<std::string> kDrop = {"BOTS_LAUNCHER", "BOTS_INPUT", "BOTS_EXEC",
                                              "BOTS_CMDLINE", "BOTS_CAPTURE_DIR"};
  std::vector<std::string> out, p2;
  for (const auto& kv : in) {
    std::string name = kv.substr(0, kv.find('='));
    if (name.rfind("BOTS_P2_", 0) == 0) {
      p2.push_back("BOTS_" + kv.substr(8));
    } else if (!kDrop.count(name)) {
      out.push_back(kv);
    }
  }
  out.insert(out.end(), p2.begin(), p2.end());
  return out;
}

bool Spawn(const std::vector<std::string>& args, const std::vector<std::string>& env) {
#if defined(_WIN32)
  std::wstring cmdline;
  for (const auto& a : args) {
    std::wstring w = std::filesystem::path(a).wstring();
    if (!cmdline.empty()) cmdline += L' ';
    cmdline += L'"';
    for (size_t i = 0; i < w.size(); ++i) {  // CommandLineToArgvW quoting
      size_t slashes = 0;
      while (i < w.size() && w[i] == L'\\') ++slashes, ++i;
      if (i == w.size()) {
        cmdline.append(slashes * 2, L'\\');
        break;
      }
      if (w[i] == L'"') {
        cmdline.append(slashes * 2 + 1, L'\\');
      } else {
        cmdline.append(slashes, L'\\');
      }
      cmdline += w[i];
    }
    cmdline += L'"';
  }
  std::wstring block;
  for (const auto& kv : env) block += std::filesystem::path(kv).wstring() + L'\0';
  block += L'\0';
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, block.data(), nullptr, &si, &pi)) {
    REXLOG_ERROR("coop: starting player 2 failed (error {})", GetLastError());
    return false;
  }
  REXLOG_INFO("coop: player 2 started (pid {})", pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  std::vector<char*> argv, envp;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  for (const auto& kv : env) envp.push_back(const_cast<char*>(kv.c_str()));
  envp.push_back(nullptr);
  pid_t pid = 0;
  int err = posix_spawn(&pid, args[0].c_str(), nullptr, nullptr, argv.data(), envp.data());
  if (err != 0) {
    REXLOG_ERROR("coop: starting player 2 failed ({})", std::strerror(err));
    return false;
  }
  REXLOG_INFO("coop: player 2 started (pid {})", pid);
  return true;
#endif
}

}  // namespace

int CoopPad() {
  return std::clamp(REXCVAR_GET(pad), 0, 4);
}

// BOTS_SPLIT_TEST (experiment): pads N -> user N as usual, and scripted input drives users 0 and 1 alike, so a second
// local player has a connected controller on a machine without pads; the second scripted pad ("2/" steps) only user 1.
class SplitTestAssignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    for (auto& u : users_) u.clear();
    for (const auto& device : devices) {
      if (device.synthetic && device.name == ScriptInputDriver::kPad2Name) {
        users_[1].push_back(device.id);  // BOTS_INPUT "2/..." steps: player 2 alone
      } else if (device.synthetic) {
        users_[0].push_back(device.id), users_[1].push_back(device.id);
      } else if (device.ordinal < users_.size()) {
        users_[device.ordinal].push_back(device.id);
      }
    }
  }
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    if (user_index < users_.size()) out = users_[user_index];
  }

 private:
  std::vector<std::vector<rex::input::DeviceId>> users_ = std::vector<std::vector<rex::input::DeviceId>>(4);
};

std::unique_ptr<rex::input::DeviceAssignment> MakePadAssignment() {
  if (std::getenv("BOTS_SPLIT_TEST")) return std::make_unique<SplitTestAssignment>();
  int pad = CoopPad();
  if (pad <= 0) return nullptr;
  return std::make_unique<PadAssignment>(uint32_t(pad - 1));
}

bool SplitScreen() {
  static const bool test = std::getenv("BOTS_SPLIT_TEST") != nullptr;
  return (REXCVAR_GET(split_screen) || test) && !REXCVAR_GET(coop) && CoopPad() <= 0;
}

int CoopPlayer() {
  return REXCVAR_GET(coop) ? std::clamp(REXCVAR_GET(coop_player), 1, 2) : 0;
}

void ApplyCoopDefaults() {
  int player = CoopPlayer();
  if (!player) return;
  // coop_pads "A,B": player 1 gets A, player 2 gets B.
  std::string pads = REXCVAR_GET(coop_pads);
  size_t comma = pads.find(',');
  std::string mine = player == 1 ? pads.substr(0, comma)
                                 : (comma == std::string::npos ? "" : pads.substr(comma + 1));
  int pad = std::atoi(mine.c_str());
  SetUnlessCommandLine("pad", std::to_string(pad > 0 ? pad : player));
  SetUnlessCommandLine("user_signin_live", "true");
  SetUnlessCommandLine("fullscreen", "false");
  SetUnlessCommandLine("window_tile", player == 1 ? "left" : "right");
  if (player == 2) {
    SetUnlessCommandLine("user_xuid", kPlayerTwoXuidText);
    SetUnlessCommandLine("user_gamertag", kPlayerTwoGamertag);
    // Both instances play the same game world; two mixes of it on one PC only double the sound.
    SetUnlessCommandLine("audio_mute", "true");
  }
  // This window's pad must keep working while the other window has focus. SDL reads the hint
  // when the input system opens the joystick subsystem.
  if (!std::getenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS")) {
#if defined(_WIN32)
    _putenv_s("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
#else
    setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 0);
#endif
  }
  REXLOG_INFO("coop: player {}, pad {}", player, CoopPad());
}

namespace {
int player_two_user = -1;  // the XAM user player 2 is signed in as, -1 if none
}

// No XN_SYS_SIGNINCHANGED for player 2: the game reads it as a profile change mid-game, disconnects both
// players and shows "multiple gamer profiles detected" (first real-pad test). BOTS_SIGNIN_NOTIFY=1 sends it.
bool NotifySignIn() {
  static const bool notify = std::getenv("BOTS_SIGNIN_NOTIFY") != nullptr;
  return notify;
}

void SignInPlayerTwo(uint32_t user_index) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  player_two_user = int(user_index);
  const auto& root = kernel->emulator()->user_data_root();
  kernel->SignInUser(user_index, kPlayerTwoXuid, kPlayerTwoGamertag,
                     root.empty() ? std::filesystem::path() : PlayerTwoPath(std::filesystem::absolute(root)),
                     NotifySignIn());
}

void SignOutPlayerTwo() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || player_two_user < 0) return;
  kernel->SignOutUser(uint32_t(player_two_user), NotifySignIn());
  player_two_user = -1;
}

void SpawnPlayerTwo(const std::filesystem::path& game_data_root,
                    const std::filesystem::path& user_data_root,
                    const std::filesystem::path& cache_root) {
  if (CoopPlayer() != 1) return;
  namespace fs = std::filesystem;
  std::vector<std::string> args = {rex::filesystem::GetExecutablePath().string()};
  // Player 1's settings (command line, bots.toml, setup window) carry over, except the per-player
  // ones; player 2's user folder has no bots.toml of its own. REX_* env values are inherited.
  static const std::set<std::string> kPerPlayer = {
      "coop", "coop_player", "pad", "game_data_root", "user_data_root", "cache_root",
      "log_file", "user_xuid", "user_gamertag", "window_tile", "launcher", "netplay_slot"};
  for (const auto& name : rex::cvar::ListFlags()) {
    auto source = rex::cvar::GetFlagSource(name);
    if (kPerPlayer.count(name) || source == rex::cvar::Source::kDefault ||
        source == rex::cvar::Source::kEnvironment) {
      continue;
    }
    args.push_back("--" + name + "=" + rex::cvar::GetFlagByName(name));
  }
  fs::path user2 = PlayerTwoPath(user_data_root), cache2 = PlayerTwoPath(cache_root);
  // Seed player 2's shader cache from player 1's, so it doesn't recompile everything.
  std::error_code ec;
  if (!fs::exists(cache2, ec) && fs::is_directory(cache_root, ec)) {
    fs::copy(cache_root, cache2, fs::copy_options::recursive, ec);
    if (ec) REXLOG_WARN("coop: copying the shader cache for player 2: {}", ec.message());
  }
  args.push_back("--coop=true");
  args.push_back("--coop_player=2");
  args.push_back("--game_data_root=" + game_data_root.string());
  args.push_back("--user_data_root=" + user2.string());
  args.push_back("--cache_root=" + cache2.string());
  if (std::string log = rex::cvar::GetFlagByName("log_file"); !log.empty()) {
    args.push_back("--log_file=" + PlayerTwoPath(log).string());
  }
  std::string joined;
  for (const auto& a : args) joined += " " + a;
  REXLOG_INFO("coop: starting player 2:{}", joined);
  Spawn(args, PlayerTwoEnvironment());
}

}  // namespace bots
