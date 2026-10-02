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
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <thread>

#include <signal.h>
#if defined(__APPLE__)
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/runtime.h>

#include "script_input.h"
#include "setup/app_paths.h"
#include "setup/settings.h"
#include "setup/setup_dialog.h"
#include "shader_progress.h"

#if defined(_WIN32)
void InstallWindowsCrashLogger();  // src/crash_win.cpp
#endif

class BotsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

 private:
  std::filesystem::path config_path_;  // bots.toml (OnConfigurePaths)

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
  // ones the setup window installed: bots.toml's game_data_root, else the per-user default folder.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(paths.config_path, ec)) paths.config_path = paths.user_data_root / "bots.toml";
    config_path_ = paths.config_path;
    if (paths.game_data_root.empty()) {
      std::string configured = bots::setup::ConfigString(config_path_, "game_data_root");
      paths.game_data_root =
          configured.empty() ? bots::setup::DefaultGameDir() : fs::path(configured);
    }
  }

  // Missing game files, or --launcher / BOTS_LAUNCHER=1: show the setup window and start the
  // runtime when the player presses Play. Otherwise start straight away.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    const char* env = std::getenv("BOTS_LAUNCHER");
    bool requested = rex::cvar::Query<bool>("launcher") || (env && std::string_view(env) == "1");
    if (!requested && bots::setup::IsGameDir(defaults.game_data_root)) return defaults;
    if (!imgui_drawer()) return defaults;  // no UI: fail the usual way (missing game files)
    REXLOG_INFO("setup: showing the setup window ({})",
                requested ? "requested" : "no game files");
    bool intro_before = rex::cvar::Query<bool>("intro_movies");
    new bots::setup::SetupDialog(
        imgui_drawer(), defaults.game_data_root, config_path_,
        [defaults, resume, intro_before](const std::filesystem::path& dir) {
          // The UE3 command line was set in OnPreSetup; follow a changed intro-movies choice.
          bool intro = rex::cvar::Query<bool>("intro_movies");
          if (intro != intro_before &&
              rex::cvar::GetFlagSource("cl") != rex::cvar::Source::kCommandLine) {
            rex::cvar::SetFlagByName("cl", intro ? "" : "-nostartupmovies");
          }
          rex::PathConfig paths = defaults;
          paths.game_data_root = dir;
          resume(paths);
        },
        [this]() { app_context().QuitFromUIThread(); });
    return std::nullopt;
  }

  // Shader precompile progress (B-053); the dialog deletes itself when done.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    new ShaderProgressDialog(
        drawer, [this]() { return runtime() ? runtime()->graphics_system() : nullptr; },
        [this]() {
          return runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system())
                           : nullptr;
        });
  }

  void OnPostLoadXexImage() override {
    ApplyDataPatches();
    const char* path = std::getenv("BOTS_DUMP_IMAGE");
    if (!path || !*path) return;
    const auto* src = runtime()->memory()->TranslateVirtual<const char*>(
        PPCImageConfig.image_base);
    std::ofstream out(path, std::ios::binary);
    out.write(src, PPCImageConfig.image_size);
    REXLOG_INFO("Dumped XEX image {:08X}+{:X} to {}", PPCImageConfig.image_base,
                PPCImageConfig.image_size, path);
  }

  // On SIGABRT (REX_FATAL), write the raw host backtrace + image slide to stderr
  // (lands in logs/run-N.out). Resolve offline:
  //   atos -o out/build/mac-arm64-relwithdebinfo/bots -l <load addr> <addrs...>
  // Recompiled frames resolve to sub_XXXXXXXX, i.e. this doubles as a guest call stack.
  // macOS only for now (Windows: the SDK's own unhandled-exception path).
#if defined(__APPLE__)
  static void CrashHandler(int sig) {
    void* frames[96];
    int n = backtrace(frames, 96);
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "\n=== bots crash: signal %d, load addr 0x%lx ===\n", sig,
                       (unsigned long)_dyld_get_image_header(0));
    write(2, buf, len);
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
  }
  void OnPostInitLogging() override {
    signal(SIGABRT, CrashHandler);
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
  //  - gpu_plugin=xenos: only backend we ship; saves passing --gpu_plugin every run.
  //  - depth_float24_convert_in_pixel_shader: the game's D24FS8 shadow/light-environment
  //    depth compares need exact 24-bit float depth; without it the player renders black
  //    in level 1 (B-035, sweep 2026-10-01).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    SetCvarDefault("depth_float24_convert_in_pixel_shader", "REX_DEPTH_FLOAT24_CONVERT_IN_PIXEL_SHADER", "true");
    // MoltenVK: plain Metal binding instead of Metal3 argument buffers for descriptor sets —
    // gameplay 18.4 → 22.1 fps on M2 Pro (B-032, run 38). Read at VkInstance creation, which
    // happens in the GPU plugin after this hook.
    SetEnvDefault("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0");
    // The front end never waits for vblank (130-250 swaps/s, B-038) and gameplay runs 32-52 fps
    // since upload hoisting. Cap at 60 (SDK patch 0004 cvar; same default as xenia-canary).
    SetCvarDefault("framerate_limit", "REX_FRAMERATE_LIMIT", "60");
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
    // setup window) or BOTS_INTRO_MOVIES=1 plays them.
    const char* v = std::getenv("BOTS_INTRO_MOVIES");
    if (!rex::cvar::Query<bool>("intro_movies") && !(v && std::string_view(v) == "1")) {
      SetEnvDefault("REX_CL", "-nostartupmovies");
    }
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

  // BOTS_INPUT=<script>: scripted controller as guest user 0 (see script_input.h).
  void OnPostSetup() override {
    SetLaunchCommandLine();
    RegisterFullscreenBind();
    const char* script = std::getenv("BOTS_INPUT");
    if (!script || !*script || !runtime() || !runtime()->input_system()) return;
    static_cast<rex::input::InputSystem*>(runtime()->input_system())
        ->AddDriver(std::make_unique<bots::ScriptInputDriver>(script));
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
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
