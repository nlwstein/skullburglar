// bots - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

#include <signal.h>
#if defined(__APPLE__)
#include <execinfo.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(__linux__)
#include <dlfcn.h>
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/xmemory.h>

#include "thread_stats.h"
#include "instr.h"
#include <rex/ui/presenter.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/runtime.h>

#include "app_quit.h"
#include "mnk_input.h"
#include "coop.h"
#include "widescreen.h"
#include "script_input.h"
#include "setup/app_paths.h"
#include "setup/settings.h"
#include "setup/setup_dialog.h"
#include "shader_progress.h"

#if defined(_WIN32)
void InstallWindowsCrashLogger();  // src/crash_win.cpp
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include "ios/touch_input.h"  // the iOS on-screen controller's input driver; src/android/android_app.cpp feeds it
#endif
#if TARGET_OS_IPHONE  // iOS and tvOS
#include "ios/ios_app.h"
#if TARGET_OS_TV
#include <SDL3/SDL_hints.h>
#else
#include "ios/touch_input.h"
#endif
#endif

namespace bots::hybrid {
void InstallOpenHooks();  // hybrid.cpp (B-199 level preload)
}
namespace bots::levelstream {
void Start(const std::filesystem::path& user_root);  // level_stream.cpp (B-226 level-start prefetch)
}

class BotsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

 private:
  std::filesystem::path config_path_;  // bots.toml (OnConfigurePaths)
  bool game_root_given_ = false;       // --game_data_root on the command line: skip the launcher

 public:

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<BotsApp>(new BotsApp(ctx, "bots",
        PPCImageConfig));
  }

  // Data patches applied to the loaded image (code patches would need codegen changes).
  // GTilingMode = 4: fixes black/garbled dynamically-lit characters (light environments vs
  // MSAA tiling) — the player renders solid black in level 1 without it. From the
  // xenia-canary game-patches "Dynamic Lighting & Shadows Shading Fix" (author: boma).
  // Disable with BOTS_NO_TILING_FIX=1.
  void ApplyDataPatches() {
    auto* mem = runtime()->memory();
    const char* off = std::getenv("BOTS_NO_TILING_FIX");
    if (!(off && *off == '1')) {
      auto* p = mem->TranslateVirtual<uint8_t*>(0x83777F4F);
      REXLOG_INFO("patch GTilingMode @83777F4F: {} -> 4", *p);
      *p = 4;
    }
  }

  // BOTS_DUMP_IMAGE=<path>: write the loaded (decrypted, decompressed) XEX image as a
  // flat big-endian blob based at PPCImageConfig.image_base. Used for string/RTTI/symbol
  // analysis (backlog B-011..B-013). Output is game data — keep it out of git.
  // First-run setup (B-061). Settings live in <user data root>/bots.toml, next to the saves, unless a
  // bots.toml sits next to the executable (dev). Without --game_data_root the game files are the
  // ones the setup window installed: bots.toml's game_data_root, else a portable install's game folder next to the
  // executable, else the per-user default folder.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    namespace fs = std::filesystem;
    std::error_code ec;
#if TARGET_OS_IPHONE
    // iOS and tvOS (experimental): everything lives in the app container. The game files go in <data>/game (imported
    // over the local network, see OnFinalizePaths), saves in <data>/user, the shader cache in Caches; <data> is
    // Documents on iOS and Caches on tvOS (bots::ios::DataDir). No launcher.
    {
#if TARGET_OS_TV
      // SDL lists the Siri Remote as a gamepad, and it connects first, so a real controller lands on the second
      // user slot, which has no signed-in profile ("no gamer profile is signed in" at the pay phones). The game
      // needs a real controller anyway. A hint, not setenv: SDL has already copied the environment by now.
      SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
#endif
      const fs::path docs = bots::ios::DataDir();
      if (paths.game_data_root.empty()) paths.game_data_root = docs / "game";
      if (rex::cvar::Query<std::string>("user_data_root").empty()) paths.user_data_root = docs / "user";
      if (rex::cvar::Query<std::string>("cache_root").empty()) paths.cache_root = bots::ios::CachesDir() / "bots";
      paths.config_path = paths.user_data_root / "bots.toml";
      config_path_ = paths.config_path;
      game_root_given_ = true;
      fs::create_directories(paths.user_data_root, ec);
      fs::create_directories(paths.cache_root, ec);
      if (rex::cvar::Query<std::string>("log_file").empty()) {
        fs::create_directories(docs / "logs", ec);
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        rex::cvar::SetFlagByName("log_file", (docs / "logs" / ("bots-" + std::string(stamp) + ".log")).string());
      }
      return;
    }
#endif
#if defined(__ANDROID__)
    // Android (experimental): everything lives in the app's own storage, and there is no launcher. <data> is the app's
    // external files folder (Android/data/<package>/files), which a computer can write over USB (adb push): the game
    // files go in <data>/game, saves in <data>/user, logs in <data>/logs; the shader cache in the internal storage.
    {
      const char* external = SDL_GetAndroidExternalStoragePath();
      const char* internal = SDL_GetAndroidInternalStoragePath();
      const fs::path data = external ? fs::path(external) : fs::path(internal ? internal : ".");
      if (paths.game_data_root.empty()) paths.game_data_root = data / "game";
      if (rex::cvar::Query<std::string>("user_data_root").empty()) paths.user_data_root = data / "user";
      if (rex::cvar::Query<std::string>("cache_root").empty())
        paths.cache_root = fs::path(internal ? internal : data.c_str()) / "cache";
      paths.config_path = paths.user_data_root / "bots.toml";
      config_path_ = paths.config_path;
      game_root_given_ = true;
      fs::create_directories(paths.user_data_root, ec);
      fs::create_directories(paths.cache_root, ec);
      if (rex::cvar::Query<std::string>("log_file").empty()) {
        fs::create_directories(data / "logs", ec);
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        rex::cvar::SetFlagByName("log_file", (data / "logs" / ("bots-" + std::string(stamp) + ".log")).string());
      }
      return;
    }
#endif
    if (!fs::exists(paths.config_path, ec)) paths.config_path = paths.user_data_root / "bots.toml";
    config_path_ = paths.config_path;
    UseBundleLogDir();
    game_root_given_ = !paths.game_data_root.empty();
    if (paths.game_data_root.empty()) {
      std::string configured = bots::setup::ConfigString(config_path_, "game_data_root");
      paths.game_data_root = configured.empty()
                                 ? bots::setup::AutoGameDir(rex::filesystem::GetExecutableFolder())
                                 : fs::path(configured);
    }
  }

  // In the macOS app bundle, log to ~/Library/Logs/Skullburglar/bots-<time>.log (newest 10 kept) unless --log_file
  // is given. Runs before the SDK opens the log file.
  static void UseBundleLogDir() {
    namespace fs = std::filesystem;
    const fs::path dir = bots::setup::BundleLogDir(rex::filesystem::GetExecutableFolder());
    if (dir.empty() || !rex::cvar::Query<std::string>("log_file").empty()) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<fs::path> old;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      const auto name = entry.path().filename().string();
      if (name.starts_with("bots-") && entry.path().extension() == ".log") old.push_back(entry.path());
    }
    std::sort(old.begin(), old.end());  // the names sort by time
    for (size_t i = 0; i + 9 < old.size(); ++i) fs::remove(old[i], ec);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    rex::cvar::SetFlagByName("log_file", (dir / ("bots-" + std::string(stamp) + ".log")).string());
  }

  // The launcher opens on every start and starts the runtime when the player presses Play. Skipped
  // with --launcher=false (or bots.toml / REX_LAUNCHER) or BOTS_LAUNCHER=0, and by default when the
  // game files are given on the command line (dev and test launches). Missing game files always
  // open it.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
#if TARGET_OS_IPHONE
    // No launcher on iOS and tvOS. Without game files, the import screen takes the player's disc image over the local
    // network; the files stay in the app's data folder across app updates.
    if (bots::setup::IsGameDir(defaults.game_data_root)) {
      bots::ios::ExcludeFromBackup(defaults.game_data_root);
      return defaults;
    }
    bots::ios::ImportGame(defaults.game_data_root, [defaults, resume] { resume(defaults); });
    return std::nullopt;
#endif
#if defined(__ANDROID__)
    // No launcher on Android: the app's import screen (tools/android ImportActivity) installs the game files into
    // <data>/game before the game starts, or they are copied there by hand (OnConfigurePaths).
    return defaults;
#endif
    bool requested = !game_root_given_;
    if (rex::cvar::GetFlagSource("launcher") != rex::cvar::Source::kDefault)
      requested = rex::cvar::Query<bool>("launcher");
    if (const char* env = std::getenv("BOTS_LAUNCHER"); env && *env)
      requested = std::string_view(env) != "0";
    if (!requested && bots::setup::IsGameDir(defaults.game_data_root)) return defaults;
    if (!imgui_drawer()) return defaults;  // no UI: fail the usual way (missing game files)
    REXLOG_INFO("setup: showing the setup window ({})",
                requested ? "requested" : "no game files");
    bool intro_before = rex::cvar::Query<bool>("intro_movies");
    SetPickerHost();
    new bots::setup::SetupDialog(
        imgui_drawer(), defaults.game_data_root,
        bots::setup::AutoGameDir(rex::filesystem::GetExecutableFolder()), game_root_given_, config_path_,
        [defaults, resume, intro_before](const std::filesystem::path& dir) {
          // The UE3 command line was set in OnPreSetup; follow a changed intro-movies choice.
          bool intro = rex::cvar::Query<bool>("intro_movies");
          const auto cl_source = rex::cvar::GetFlagSource("cl");
          if (intro != intro_before && cl_source != rex::cvar::Source::kCommandLine &&
              cl_source != rex::cvar::Source::kEnvironment) {
            rex::cvar::SetFlagByName("cl", intro ? "" : "-nostartupmovies");
          }
          ApplyVsync();  // the game's swapchain doesn't exist yet
          bots::ApplyCoopDefaults();  // co-op may have been switched on in the launcher
          rex::PathConfig paths = defaults;
          paths.game_data_root = dir;
          resume(paths);
        },
        [this]() { app_context().QuitFromUIThread(); });
    return std::nullopt;
  }

  // The launcher's file dialogs (file_picker.h) belong to the game window, and a fullscreen window steps out of
  // fullscreen while one is open: a fullscreen window kept them out of sight. macOS keeps its own panel in front.
  void SetPickerHost() {
#if !defined(__APPLE__)
    bots::setup::SetPickerHost({window() ? window()->GetNativeWindowHandle() : nullptr, [this](bool open) {
                                  rex::ui::Window* w = window();
                                  if (!w) return;
                                  if (open) {
                                    picker_left_fullscreen_ = w->IsFullscreen();
                                    if (picker_left_fullscreen_) w->SetFullscreen(false);
                                  } else if (picker_left_fullscreen_) {
                                    picker_left_fullscreen_ = false;
                                    w->SetFullscreen(true);
                                  }
                                }});
#endif
  }
  bool picker_left_fullscreen_ = false;

  // Shader precompile progress (B-053); the dialog deletes itself when done.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    new ShaderProgressDialog(
        drawer, [this]() { return runtime() ? runtime()->graphics_system() : nullptr; },
        [this]() {
          return runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system())
                           : nullptr;
        });
  }

  // The SDK's achievement toast keeps an ImGui dialog registered while it shows, which puts the presenter in
  // vblank-paced UI-thread painting: the frame rate drops and stutters for the toast's few seconds. Off unless
  // the player asks for it (achievement_popups); unlocks are still recorded and listed in the overlay.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    if (!rex::cvar::Query<bool>("achievement_popups")) return nullptr;
    return rex::ReXApp::CreateAchievementNotificationDialog();
  }

  void OnPostLoadXexImage() override {
    ApplyDataPatches();
    bots::hybrid::InstallOpenHooks();  // B-199: see the packages the game opens from the first one
    const char* path = std::getenv("BOTS_DUMP_IMAGE");
    if (!path || !*path) return;
    const auto* src = runtime()->memory()->TranslateVirtual<const char*>(
        PPCImageConfig.image_base);
    std::ofstream out(path, std::ios::binary);
    out.write(src, PPCImageConfig.image_size);
    REXLOG_INFO("Dumped XEX image {:08X}+{:X} to {}", PPCImageConfig.image_base,
                PPCImageConfig.image_size, path);
  }

  // On SIGABRT (REX_FATAL, or an unhandled host fault: the SDK's handler aborts) or SIGBUS, write the raw
  // host backtrace + image slide to stderr (lands in logs/run-N.out). Resolve offline:
  //   macOS: atos -o out/build/mac-arm64-relwithdebinfo/bots -l <load addr> <addrs...>
  //   Linux: frames print as module(+offset); addr2line -f -C -e <module> <offset>
  // Recompiled frames resolve to sub_XXXXXXXX, i.e. this doubles as a guest call stack.
  // Windows: the SDK's own unhandled-exception path. Android: none (no backtrace() before API 33); the system's
  // crash report (logcat, tombstones) has the native backtrace.
#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
  static void CrashHandler(int sig) {
    void* frames[96];
    int n = backtrace(frames, 96);
    char buf[64];
#if defined(__APPLE__)
    const unsigned long load = (unsigned long)_dyld_get_image_header(0);
#else
    Dl_info info{};
    dladdr(reinterpret_cast<void*>(&CrashHandler), &info);
    const unsigned long load = (unsigned long)info.dli_fbase;
#endif
    int len = snprintf(buf, sizeof(buf), "\n=== bots crash: signal %d, load addr 0x%lx ===\n", sig, load);
    write(2, buf, len);
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
  }
  void OnPostInitLogging() override {
    signal(SIGABRT, CrashHandler);
#if defined(__linux__)
    signal(SIGBUS, CrashHandler);  // the SDK claims SIGBUS on macOS only
#endif
  }
#elif defined(_WIN32)
  void OnPostInitLogging() override { InstallWindowsCrashLogger(); }
#endif

  // A built-in default for an SDK cvar, as REX_<NAME> (see OnPreSetup): skipped when bots.toml
  // sets the cvar, since env outranks config and would otherwise hide the player's setting.
  void SetCvarDefault(const char* cvar, const char* env, const char* value) {
    if (bots::setup::ConfigHasKey(config_path_, cvar)) return;
    if (std::getenv(env)) return;  // a real launch option
    SetEnvDefault(env, value);
    bots::setup::MarkAppDefault(cvar);
  }

  // The same for a cvar the SDK registered before this hook (REX_* is not replayed for those): set it unless the
  // command line, the environment or bots.toml chose a value.
  void SetCoreCvarDefault(const char* cvar, const char* value) {
    if (bots::setup::ConfigHasKey(config_path_, cvar)) return;
    if (rex::cvar::GetFlagSource(cvar) != rex::cvar::Source::kDefault) return;
    rex::cvar::SetFlagByName(cvar, value);
    bots::setup::MarkAppDefault(cvar);
  }

  // The present_vsync setting (bots.toml / launcher, default on) picks the Vulkan present mode through the SDK's
  // vulkan_allow_present_mode_* cvars, read when the swapchain is created: on = FIFO (the SDK then paints on the UI
  // thread at each vblank, so the guest thread never waits for the display), off = IMMEDIATE (can tear). A present
  // mode cvar given on the command line or as REX_* wins. Not the SDK's `vsync`, which paces the guest's vblank.
  static void ApplyVsync() {
    const bool on = rex::cvar::Query<bool>("present_vsync");
    for (const char* cvar : {"vulkan_allow_present_mode_immediate", "vulkan_allow_present_mode_mailbox",
                             "vulkan_allow_present_mode_fifo_relaxed"}) {
      const auto source = rex::cvar::GetFlagSource(cvar);
      if (source == rex::cvar::Source::kEnvironment || source == rex::cvar::Source::kCommandLine) continue;
      rex::cvar::SetFlagByName(cvar, on ? "false" : "true");
    }
    REXLOG_INFO("vsync {}", on ? "on (FIFO)" : "off (immediate)");
  }

  // setenv(name, value, 0): set only if not already set.
  static void SetEnvDefault(const char* name, const char* value) {
    if (std::getenv(name)) return;
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
  }

  // Per-game GPU defaults. Set as REX_* env vars (overwrite=0) because the GPU plugin's
  // cvars register after this hook and replay env values then; an explicit env var or
  // --flag still wins. (A bots.toml config value loses to these — use the flag instead.)
  //  - gpu_plugin=native: our own Vulkan renderer (src/gfx). xenos, the SDK's emulated Xbox 360
  //    GPU, stays selectable (launcher "Renderer", bots.toml or --gpu_plugin=xenos).
  //  - depth_float24_convert_in_pixel_shader: the game's D24FS8 shadow/light-environment
  //    depth compares need exact 24-bit float depth; without it the player renders black
  //    in level 1 (B-035, sweep 2026-10-01).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    bots::ApplyCoopDefaults();  // --coop: before the window, input and profile read their cvars
    // The menus' Quit option (game_hooks.cpp) runs on a guest thread. Quit the way closing the window does
    // (ReXApp::OnClosing terminates the title and exits); QuitFromUIThread alone leaves the guest threads running.
    bots::SetQuitHandler([this] {
      app_context().CallInUIThreadDeferred([this] {
        if (window()) window()->RequestClose();
        else app_context().QuitFromUIThread();
      });
    });
#if defined(__APPLE__) || defined(__linux__)
    // Open files: package files stay open across level loads (~14 more per LoadChapter; whether the game or the SDK's
    // handle closing keeps them is still open). At macOS's
    // default soft limit of 256 (any app started from Finder or launchd) every open fails after ~60 loads, and the game
    // shows its dirty-disc screen. Raise the soft limit to the hard one (macOS caps it at OPEN_MAX).
    if (rlimit lim; getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur < lim.rlim_max) {
      rlim_t want = lim.rlim_max;
#if defined(__APPLE__)
      want = std::min<rlim_t>(want, OPEN_MAX);
#endif
      if (want > lim.rlim_cur) {
        lim.rlim_cur = want;
        setrlimit(RLIMIT_NOFILE, &lim);
      }
    }
#endif
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "native";
      rex::cvar::SetFlagByName("gpu_plugin", "native");  // so the launcher shows the choice in use
    }
    SetCvarDefault("depth_float24_convert_in_pixel_shader", "REX_DEPTH_FLOAT24_CONVERT_IN_PIXEL_SHADER", "true");
    // MoltenVK: plain Metal binding instead of Metal3 argument buffers for descriptor sets —
    // gameplay 18.4 → 22.1 fps on M2 Pro (B-032, run 38). Read at VkInstance creation, which
    // happens in the GPU plugin after this hook.
    SetEnvDefault("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0");
    // The front end never waits for vblank (130-250 swaps/s, B-038) and gameplay runs 32-52 fps
    // since upload hoisting. Cap at 60 (SDK patch 0004 cvar; same default as xenia-canary).
    SetCvarDefault("framerate_limit", "REX_FRAMERATE_LIMIT", "60");
    // LAN co-op (B-057): the game's co-op menus are Live-only, and the system-link hooks turn every session into a LAN
    // one, so a "Live" sign-in is what lets Join COOP find another computer's story (and gives a host's story the slots
    // a joiner needs). On by default so LAN co-op needs no setup; the launcher's "LAN co-op" setting turns it off. A core
    // SDK cvar, registered before this hook, so it is set directly rather than through REX_* like the ones above.
    SetCoreCvarDefault("user_signin_live", "true");
    ApplyVsync();
    // D3D12 (Windows): the ROV render-target path does exact 24-bit float depth in the shader, the
    // D3D12 counterpart of the float24 fix above. The default host-RTV path renders characters black
    // like B-035. RTX 3090: locked 60 fps either way (2026-10-01).
    SetCvarDefault("render_target_path_d3d12", "REX_RENDER_TARGET_PATH_D3D12", "rov");
#if defined(__linux__)
    // Vulkan counterpart on Linux: the host-RT (fbo) path draws the player as a dark-red silhouette on NVIDIA
    // (GTX 1070, B-063); fragment shader interlock is correct and faster. The SDK falls back to fbo where
    // interlock is missing. Not on macOS: MoltenVK's path and its shader master (fbo.vk) stay as they are.
    SetCvarDefault("render_target_path_vulkan", "REX_RENDER_TARGET_PATH_VULKAN", "fsi");
#endif
    // Vulkan's async mode otherwise drops whole frames that used a not-yet-created pipeline (no
    // flashing, but the picture freezes while shaders compile). D3D12 just skips those draws;
    // do the same so the background shader precompile (B-053) never freezes the screen.
    SetCvarDefault("vulkan_async_skip_incomplete_frames", "REX_VULKAN_ASYNC_SKIP_INCOMPLETE_FRAMES", "false");
    // Skip the Legal / THQ / Swordfish logo movies: UE3's own -nostartupmovies switch, passed on the
    // guest command line (SDK cvar `cl` -> ExLoadedCommandLine). The intro_movies setting (bots.toml,
    // setup window) or BOTS_INTRO_MOVIES=1 plays them. Set on the cvar itself: `cl` is a static cvar whose REX_CL was
    // read before this hook, so an env default here would be ignored. A --cl or REX_CL given by hand wins.
    // The shader precompile (OnCreateDialogs) doesn't depend on this; it runs either way.
    const char* v = std::getenv("BOTS_INTRO_MOVIES");
    const auto cl_source = rex::cvar::GetFlagSource("cl");
    if (!rex::cvar::Query<bool>("intro_movies") && !(v && std::string_view(v) == "1") &&
        cl_source != rex::cvar::Source::kCommandLine && cl_source != rex::cvar::Source::kEnvironment) {
      rex::cvar::SetFlagByName("cl", "-nostartupmovies");
    }
    // Co-op on one PC (B-057): with --pad, this instance's pad must keep working while the other
    // instance's window has focus (--coop sets it in ApplyCoopDefaults). SDL reads the hint when it
    // opens the joystick subsystem.
    if (bots::CoopPad() > 0) SetEnvDefault("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
  }

  // BOTS_CMDLINE="<args>": UE3 command line. The game reads XAM launch data as an ANSI string
  // and uses "dummy " + data as appCmdLine (sub_82359E38, stock UE3 Xenon startup), so this
  // takes a map URL and/or switches, e.g. "Ambush_P?LoadChapter=11010" (debug/testing aid).
  void SetLaunchCommandLine() {
    const char* cmdline = std::getenv("BOTS_CMDLINE");
    if (!cmdline || !*cmdline || !runtime() || !runtime()->kernel_state()) return;
    auto xam = runtime()->kernel_state()->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
    if (!xam) return;
    auto& loader_data = xam->loader_data();
    loader_data.launch_data.assign(cmdline, cmdline + std::strlen(cmdline) + 1);
    loader_data.launch_data_present = true;
    REXLOG_INFO("launch data command line: {}", cmdline);
  }

  // F11 (rebindable: bind_fullscreen) toggles the SDK's `fullscreen` cvar; its change callback
  // (ReXApp::SetupPresentation) switches the window live. Start windowed with --fullscreen=false.
  void RegisterFullscreenBind() {
    rex::ui::RegisterBind("bind_fullscreen", "F11", "Toggle fullscreen", [] {
      bool on = rex::cvar::Query<bool>("fullscreen");
      rex::cvar::SetFlagByName("fullscreen", on ? "false" : "true");
    });
  }

  // Keyboard and mouse (mnk_input.h), and BOTS_INPUT=<script>: scripted controller as guest user 0
  // (script_input.h). --pad=N: only pad N drives this instance; --coop: player 1 starts player 2 here,
  // once the launcher (if any) has settled the game files and before the shader cache opens (coop.h).
  void OnPostSetup() override {
    bots::StartThreadStats();
    SetLaunchCommandLine();
    RegisterFullscreenBind();
    bots::SpawnPlayerTwo(game_data_root(), user_data_root(), cache_root());
    bots::levelstream::Start(user_data_root());  // level_stream.cpp: level-start prefetch (B-226)
    if (runtime()) bots::SetWidescreenWindow(runtime()->display_window());
    // BOTS_INSTR: status stream and control channel for tests and agents (instr.h).
    if (bots::instr::Enabled())
      bots::instr::Start([this] { return runtime() ? runtime()->graphics_system() : nullptr; });
    if (!runtime() || !runtime()->input_system()) return;
    auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    if (auto assignment = bots::MakePadAssignment()) {
      REXLOG_INFO("input: pad {} drives player one", bots::CoopPad());
      input->SetDeviceAssignment(std::move(assignment));
    }
    // Added after the SDK attached the window and its overlay callback to the drivers it had, so do
    // both here. ImGui dialogs (settings/console/debug overlays, shader progress) exist only while
    // shown: release the pointer while any that takes input is up. Passive ones (achievement toast,
    // backgrounded shader progress bar) don't count, or each toast froze keyboard/mouse play.
    auto kbm = std::make_unique<bots::MouseKeyboardDriver>();
    kbm->set_is_active_callback([drawer = runtime()->imgui_drawer()] { return !drawer || !drawer->HasInputDialogs(); });
    if (auto* window = runtime()->display_window()) kbm->OnWindowAvailable(window);
    input->AddDriver(std::move(kbm));
    const char* script = std::getenv("BOTS_INPUT");
    if (script && *script) input->AddDriver(std::make_unique<bots::ScriptInputDriver>(script));
    if (bots::instr::Enabled()) input->AddDriver(bots::instr::MakeInputDriver());
#if TARGET_OS_IPHONE
#if !TARGET_OS_TV
    // On-screen controller, shown while no game controller is connected.
    input->AddDriver(std::make_unique<bots::ios::TouchInputDriver>());
#endif
    bots::ios::InstallAppUI();
#endif
#if defined(__ANDROID__)
    // On-screen controller (tools/android TouchPadView), shown while no game controller is connected.
    input->AddDriver(std::make_unique<bots::ios::TouchInputDriver>());
#endif
  }

  // BOTS_CAPTURE_DIR=<dir> [BOTS_CAPTURE_SECS=5]: periodically save the guest output
  // (what the game presented) as <dir>/frame-NNNN.ppm — lets an unattended agent see
  // whether anything renders without screenshotting the desktop. Debug aid only.
  void OnPostLaunchModule(rex::system::XThread*) override {
    const char* dir = std::getenv("BOTS_CAPTURE_DIR");
    if (!dir || !*dir) return;
    const char* secs_env = std::getenv("BOTS_CAPTURE_SECS");
    int secs = secs_env ? std::max(1, std::atoi(secs_env)) : 5;
    std::filesystem::create_directories(dir);
    std::thread([this, dir = std::string(dir), secs] {
      for (int n = 0;; ++n) {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        auto* gfx = runtime() ? runtime()->graphics_system() : nullptr;
        auto* presenter = gfx ? gfx->presenter() : nullptr;
        rex::ui::RawImage img;
        if (!presenter || !presenter->CaptureGuestOutput(img) || !img.width) {
          REXLOG_INFO("capture {}: no guest output", n);
          continue;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "/frame-%04d.ppm", n);
        std::ofstream out(dir + name, std::ios::binary);
        out << "P6\n" << img.width << " " << img.height << "\n255\n";
        for (uint32_t y = 0; y < img.height; ++y) {
          const uint8_t* row = img.data.data() + y * img.stride;
          for (uint32_t x = 0; x < img.width; ++x) out.write(reinterpret_cast<const char*>(row + x * 4), 3);
        }
        REXLOG_INFO("capture {}: {}x{} -> {}{}", n, img.width, img.height, dir, name);
      }
    }).detach();
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
