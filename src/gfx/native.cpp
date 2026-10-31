// The native renderer as a GPU plugin (librexgpu-native, selected with REX_GPU_PLUGIN=native or --gpu_plugin
// native). The SDK's command processor still consumes the runtime's PM4 ring on its own thread, and with it the
// CPU/GPU handshakes the runtime relies on (scratch-register writeback, interrupts to the guest's callback,
// WAIT_REG_MEM, coherency, gamma-ramp registers, occlusion-query events); draws, resolves and presents go to the
// native renderer instead of the emulated EDRAM backend. A plugin rather than part of the game so its SDK sources
// and their cvars live in their own module, as the xenos plugin's do.
#include "../path_utf8.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_misc.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/registers.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/presenter.h>

#include "../instr_state.h"
#include <rex/ui/vulkan/provider.h>

#include "../hybrid_state.h"
#include "host_stats.h"
#include "start_failure.h"
#include "pm4.h"
#include "upk.h"
#include "vk/memory_sync.h"
#include "vk/renderer.h"

namespace bots::gfx {

// The renderer couldn't start: tell the player which GPU and why (the reason is in the log too), with the log one
// click away, then quit. Nothing else would show: without a renderer the window stays black. BOTS_NO_DIALOGS=1
// skips the box (unattended runs).
[[noreturn]] void ShowStartFailure(const std::string& gpu, const std::string& reason, const std::string& advice) {
  const std::string log = rex::cvar::GetFlagByName("log_file");
  if (const char* v = std::getenv("BOTS_NO_DIALOGS"); !(v && *v == '1')) {
    std::string text = "Skullburglar couldn't start its graphics.\n\nGraphics card: " + gpu + "\nReason: " + reason +
                       (advice.empty() ? "\n\nThis usually means the graphics driver is out of date, or the game picked a "
                                         "different graphics card than you meant it to (for example the one built into "
                                         "the processor). Updating the driver from the maker's website often fixes it."
                                       : "\n\n" + advice);
    if (!log.empty()) text += "\n\nThe log, with more detail, is at:\n" + log;
    // No launcher (iOS, tvOS, Android): the box itself offers the pre-1.0 renderer (HybridState::pick_pre1), and there
    // is no program to open the log with.
    const HybridState* hs = FindHybridState();
    void (*pick_pre1)() = hs ? hs->pick_pre1.load() : nullptr;
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Open log"},
                                                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"}};
    const SDL_MessageBoxButtonData pick_buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, "Use the pre-1.0 renderer"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"}};
    SDL_MessageBoxData box{SDL_MESSAGEBOX_ERROR, nullptr, "Skullburglar", text.c_str(), SDL_arraysize(buttons), buttons,
                           nullptr};
    if (pick_pre1) box.numbuttons = SDL_arraysize(pick_buttons), box.buttons = pick_buttons;
    int choice = 0;
    if (pick_pre1) {
      if (SDL_ShowMessageBox(&box, &choice) && choice == 2) pick_pre1();
      std::_Exit(1);
    }
    while (SDL_ShowMessageBox(&box, &choice) && choice == 1 && !log.empty()) {
      // The log file itself, else its folder (some systems have no program for .log files).
      auto url = [](const std::filesystem::path& p) {
        std::string path = bots::PathUtf8(p);
#if defined(_WIN32)
        std::replace(path.begin(), path.end(), '\\', '/');
#endif
        return bots::FileUrl(path);
      };
      const std::filesystem::path path = bots::PathFromUtf8(log);
      if (!SDL_OpenURL(url(path).c_str())) SDL_OpenURL(url(path.parent_path()).c_str());
    }
  }
  std::_Exit(1);
}

namespace {

namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

class NativeCommandProcessor final : public rg::CommandProcessor {
 public:
  NativeCommandProcessor(rg::GraphicsSystem* graphics_system, rex::system::KernelState* kernel_state)
      : CommandProcessor(graphics_system, kernel_state) {}
  ~NativeCommandProcessor() override { StopHostReports(); }

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override {
    if (!renderer_) return;
    TrackFrame();
    renderer_->EndFrame();
    PublishWarmup();
    HoldForWarmup();
    if (InstrState* is = FindInstrState()) PublishInstr(*is, frontbuffer_width, frontbuffer_height);
    ++frames_;
    if (bots::HybridState* hs = bots::FindHybridState(); hs && hs->fps_counter.load(std::memory_order_relaxed))
      hs->swaps.fetch_add(1, std::memory_order_relaxed);  // the in-game FPS counter (fps_counter.h)
    // BOTS_GFX_NO_PRESENT=1 (benchmarking): render everything but never hand the frame to the presenter. Under a
    // GPU-less X server (Xvfb) the Vulkan surface falls back to xcb_put_image and presenting costs more than the
    // whole frame's rendering, which hides the renderer's own throughput.
    static const bool no_present = std::getenv("BOTS_GFX_NO_PRESENT") != nullptr;
    rex::ui::Presenter* presenter = no_present ? nullptr : graphics_system_->presenter();
    const uint64_t presents = renderer_->stats().presents;
    if (presenter) {
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      const uint32_t width = frontbuffer_width ? frontbuffer_width : uint32_t(video_mode.display_width);
      const uint32_t height = frontbuffer_height ? frontbuffer_height : uint32_t(video_mode.display_height);
      // Above render scale 1 the guest output is the rendered size (e.g. 2560x1440 at 2); the presenter scales it to
      // the window with the display's aspect ratio either way.
      const float output_scale = renderer_->output_scale();
      const uint32_t out_width = uint32_t(float(width) * output_scale + 0.5f);
      const uint32_t out_height = uint32_t(float(height) * output_scale + 0.5f);
      // The presenter fits the output to the window at the display's aspect ratio. A frame wider than that is the
      // ultrawide back buffer (src/widescreen.h, e.g. 1280x548): fit it at its own shape instead of squeezing it.
      uint32_t aspect_w = std::max(1u, uint32_t(video_mode.display_width));
      uint32_t aspect_h = std::max(1u, uint32_t(video_mode.display_height));
      if (uint64_t(width) * aspect_h > uint64_t(aspect_w) * height) aspect_w = width, aspect_h = height;
      const auto* ramp = reinterpret_cast<const uint32_t*>(gamma_ramp_256_entry_table());
      presenter->RefreshGuestOutput(
          out_width, out_height, aspect_w, aspect_h,
          [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& vulkan_context =
                static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
            context.SetIs8bpc(true);
            return renderer_->Present(frontbuffer_ptr & 0x1FFFFFFF, vulkan_context.image(),
                                      vulkan_context.image_view(), out_width, out_height, ramp);
          });
    }
    // Present submits the frame. When it didn't run - no presenter, or the presenter skipped the refresh (window
    // hidden or minimised, display asleep) - submit here, or the next frames pile into one command buffer and the
    // GPU work arrives in bursts.
    if (renderer_->stats().presents == presents) renderer_->Flush();
    if (frames_ % 600 == 1) {
      const auto& st = renderer_->stats();
      // Per-frame averages over the last interval (P5 profiling): wall time on this thread, and how much of it
      // was spent recording draws (incl. textures and flushes), loading textures, and blocked on the GPU.
      const auto now = std::chrono::steady_clock::now();
      const double n = double(frames_ - last_.frames);
      if (last_.frames && n > 0) {
        auto ms = [&](uint64_t now_us, uint64_t then_us) { return double(now_us - then_us) / 1000.0 / n; };
        REXLOG_INFO(
            "gfx native: per frame over {} frames: {:.2f} ms wall, {:.2f} ms in draws, {:.2f} ms textures, {:.2f} ms "
            "GPU wait, {:.1f} flushes, {:.0f} draws, {:.1f} resolves, {:.1f} render passes, {:.1f} transfers, {:.1f} barriers, "
            "{:.1f} draws waiting on shaders",
            frames_ - last_.frames,
            std::chrono::duration<double, std::milli>(now - last_.time).count() / n, ms(st.draw_us, last_.draw_us),
            ms(st.texture_us, last_.texture_us), ms(st.wait_us, last_.wait_us), double(st.flushes - last_.flushes) / n,
            double(st.draws - last_.draws) / n, double(st.resolves - last_.resolves) / n,
            double(st.render_passes - last_.render_passes) / n, double(st.transfers - last_.transfers) / n,
            double(st.barriers - last_.barriers) / n, double(st.pending_draws - last_.pending_draws) / n);
        if (renderer_->draw_profile()) {
          std::string phases;
          for (size_t i = 0; i < std::size(st.draw_phase_ns); ++i) {
            phases += fmt::format(" {} {:.2f},", vk::Renderer::kDrawPhaseNames[i],
                                  double(st.draw_phase_ns[i] - last_.draw_phase_ns[i]) / 1e6 / n);
          }
          REXLOG_INFO("gfx native: draw CPU ms per frame:{} index buffers {:.0f} reused, {:.0f} converted, "
                      "{:.1f} invalidations",
                      phases, double(st.index_reuses - last_.index_reuses) / n,
                      double(st.index_converts - last_.index_converts) / n,
                      double(st.invalidations - last_.invalidations) / n);
        }
        if (renderer_->gpu_timing()) {
          std::string gpu;
          double total = 0;
          for (size_t i = 0; i < std::size(st.gpu_ns); ++i) {
            const double t = double(st.gpu_ns[i] - last_.gpu_ns[i]) / 1e6 / n;
            total += t;
            gpu += fmt::format(" {} {:.2f},", vk::Renderer::kGpuCatNames[i], t);
          }
          REXLOG_INFO("gfx native: GPU ms per frame: {:.2f} total:{}", total, gpu);
          REXLOG_INFO("gfx native: top GPU spans per frame:{}", renderer_->TakeGpuLabelReport(n, 25));
        }
        REXLOG_INFO("gfx native: CP idle {:.2f} ms per frame, {:.2f} ms of it after submitting queries",
                    double(idle_us_ - last_idle_us_) / 1000.0 / n,
                    double(query_idle_us_ - last_query_idle_us_) / 1000.0 / n);
        ReportFrameTimes(frames_ - last_.frames);
        REXLOG_INFO("gfx native: textures {} held, {:.0f} MB (budget {}); over {} frames: {} loaded ({} of them "
                    "evicted earlier), {} reloaded after CPU writes, {} evicted",
                    renderer_->texture_count(), renderer_->texture_bytes() / 1048576.0,
                    renderer_->texture_budget() ? fmt::format("{:.0f} MB", renderer_->texture_budget() / 1048576.0)
                                                : std::string("none, evicted by age"),
                    frames_ - last_.frames, st.texture_loads - last_.texture_loads,
                    st.texture_loads_evicted - last_.texture_loads_evicted, st.texture_reloads - last_.texture_reloads,
                    st.texture_evictions - last_.texture_evictions);
        RequestHostReport(now);
      }
      REXLOG_INFO("gfx native: memory: {}", renderer_->TakeMemoryReport());
      last_idle_us_ = idle_us_, last_query_idle_us_ = query_idle_us_;
      last_ = {frames_, now, st.draw_us, st.texture_us, st.wait_us, st.flushes, st.draws, st.resolves};
      last_.render_passes = st.render_passes, last_.transfers = st.transfers, last_.barriers = st.barriers;
      last_.pending_draws = st.pending_draws;
      last_.index_reuses = st.index_reuses, last_.index_converts = st.index_converts;
      last_.invalidations = st.invalidations;
      last_.texture_loads = st.texture_loads, last_.texture_loads_evicted = st.texture_loads_evicted;
      last_.texture_reloads = st.texture_reloads, last_.texture_evictions = st.texture_evictions;
      renderer_->SavePipelineCache();  // runs are often killed rather than shut down
      std::copy(std::begin(st.gpu_ns), std::end(st.gpu_ns), last_.gpu_ns);
      std::copy(std::begin(st.draw_phase_ns), std::end(st.draw_phase_ns), last_.draw_phase_ns);
      REXLOG_INFO(
          "gfx native: frame {}: {} draws, {} resolves, {} presents, {} failed shaders, {} unsupported prims, "
          "{} unsupported textures, {:.1f} MB uploaded, {} invalidations, {} texture reloads, {} evictions, {} indirect draws skipped, "
          "{} draws skipped for pipelines, {}/{} pipelines warmed, {} built during play ({} new shader, {} new pair, "
          "{} other state, {} not warmed yet), {} failed in the driver ({} draws dropped), {} occlusion queries ({} "
          "uncounted, {} hidden) (totals)",
          frames_, st.draws, st.resolves, st.presents, st.failed_shaders, st.unsupported_primitives,
          st.unsupported_textures, st.uploaded_bytes / 1048576.0, st.invalidations, st.texture_reloads,
          st.texture_evictions, indirect_draws_, st.pending_draws, renderer_->warmup_done(),
          renderer_->warmup_total(), st.live_pipelines, st.live_pipelines_new_shader, st.live_pipelines_new_pair,
          st.live_pipelines - st.live_pipelines_new_shader - st.live_pipelines_new_pair - st.live_pipelines_queued,
          st.live_pipelines_queued, renderer_->pipeline_failures(), st.failed_pipeline_draws, st.occlusion_queries, st.occlusion_inexact, renderer_->occlusion_hidden());
    }
  }

 protected:
  bool SetupContext() override {
    auto* provider = static_cast<rex::ui::vulkan::VulkanProvider*>(graphics_system_->provider());
    if (!provider) {
      REXLOG_ERROR("gfx native: no usable Vulkan graphics device");
      ShowStartFailure("none found", "no graphics card with a working Vulkan driver was found");
    }
    rex::memory::Memory* memory = memory_;
    renderer_ = std::make_unique<vk::Renderer>(provider->vulkan_device(), [memory](uint32_t address) {
      return memory->TranslatePhysical<const uint8_t*>(address);
    });
    if (!renderer_->ok()) {
      const std::string gpu = renderer_->DeviceDescription(), reason = renderer_->failure();
      REXLOG_ERROR("gfx native: renderer initialisation failed on {}: {}", gpu, reason);
      renderer_.reset();
      ShowStartFailure(gpu, reason);
      return false;
    }
    sync_ = std::make_unique<vk::MemorySync>(*memory_, *renderer_);
    renderer_->SetMemoryTracker(sync_.get());
    // Shipped shader refs (B-098): next to the executable in pipelines/ (Contents/Resources/pipelines in the macOS
    // app bundle), read from the player's disc and XEX image.
    auto package = std::make_shared<std::pair<std::string, std::unique_ptr<upk::Package>>>();
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    std::vector<std::filesystem::path> refs_dirs{exe_dir / "pipelines"};
    if (exe_dir.filename() == "MacOS") refs_dirs.push_back(exe_dir.parent_path() / "Resources" / "pipelines");
    renderer_->SetShaderRefs(
        std::move(refs_dirs),
        [this, package](const std::string& source, uint32_t offset, uint32_t size, std::vector<uint8_t>& out) {
          if (source == "image") {
            auto module = kernel_state_->GetExecutableModule();
            const auto* xex = module ? module->xex_module() : nullptr;
            if (!xex || uint64_t(offset) + size > xex->image_size()) return false;
            const auto* bytes = memory_->TranslateVirtual<const uint8_t*>(xex->base_address() + offset);
            out.assign(bytes, bytes + size);
            return true;
          }
          if (package->first != source || !package->second) {
            package->first = source;
            package->second = std::make_unique<upk::Package>(kernel_state_->emulator()->game_data_root() / source);
          }
          return package->second->Read(offset, size, out);
        });
    renderer_->SetN1Dirs(n1_cooked_dir(), n1_cache_dir_);
    if (!pipeline_cache_path_.empty()) renderer_->LoadPipelineCache(pipeline_cache_path_);
    PublishWarmup();
    const char* occlusion = std::getenv("BOTS_GFX_OCCLUSION");
    if (occlusion && *occlusion ? *occlusion == '1' : renderer_->occlusion_queries_by_default()) {
      renderer_->EnableOcclusionQueries(1000);  // "visible": the SDK's fake count
    }
    REXLOG_INFO("gfx native: occlusion queries {}", renderer_->occlusion_queries() ? "on" : "off");
    REXLOG_INFO("gfx native: texture cache budget {}",
                renderer_->texture_budget() ? fmt::format("{} MB", renderer_->texture_budget() / 1048576)
                                            : std::string("none (evicted by age)"));
    REXLOG_INFO("gfx native: graphics pipeline library: {}", renderer_->PipelineLibrarySupport());
    REXLOG_INFO("gfx native: renderer up");
    return true;
  }

  // Runs on the command processor thread, or before the context is set up while the processor is paused.
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) override {
    pipeline_cache_path_ = cache_root / "native" / fmt::format("{:08X}.pipelines.bin", title_id);
    n1_cache_dir_ = bots::PathUtf8(cache_root / "n1");
    if (renderer_) {
      renderer_->SetN1Dirs(n1_cooked_dir(), n1_cache_dir_);
      renderer_->LoadPipelineCache(pipeline_cache_path_);
      PublishWarmup();
    }
  }

  void ShutdownContext() override {
    StopHostReports();
    if (renderer_) renderer_->SetMemoryTracker(nullptr);
    sync_.reset();
    renderer_.reset();
  }

  rg::Shader* LoadShader(xe::ShaderType shader_type, uint32_t guest_address, const uint32_t* host_address,
                         uint32_t dword_count) override {
    const bool vertex = shader_type == xe::ShaderType::kVertex;
    ShaderLoad& load = vertex ? vs_ : ps_;
    if (host_address == memory_->TranslatePhysical<const uint32_t*>(guest_address)) {
      load = {guest_address, dword_count, nullptr};
    } else {
      // IM_LOAD_IMMEDIATE: the microcode is in the ring, which moves on before the draw.
      std::vector<uint32_t>& copy = vertex ? vs_inline_ : ps_inline_;
      copy.assign(host_address, host_address + dword_count);
      load = {0, dword_count, copy.data()};
    }
    return nullptr;
  }

  // The ring kicks the runtime's command buffers as indirect buffers (depth 1: the stream captures record).
  // Inside those, nested indirect buffers hold predicated-tiling replays (draws and resolves already issued,
  // once per tile; the renderer does each once over the whole surface) and the XDK's pipeline flush (point
  // draws with colour writes off). Their sync packets still run; their draws are dropped, as the replay
  // executor (pm4.h) does.
  void ExecuteIndirectBuffer(uint32_t ptr, uint32_t length) override {
    ++indirect_depth_;
    CommandProcessor::ExecuteIndirectBuffer(ptr, length);
    --indirect_depth_;
  }

  bool IssueDraw(xe::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override {
    if (!renderer_) return false;
    if (indirect_depth_ > 1) {
      ++indirect_draws_;
      return true;
    }
    Draw draw;
    draw.constant_writes = constant_writes_.writes();
    draw.initiator.value = register_file_->values[rg::XE_GPU_REG_VGT_DRAW_INITIATOR];
    if (index_buffer_info) {
      draw.index_base = index_buffer_info->guest_base;
      draw.index_words = uint32_t(index_buffer_info->length /
                                  (index_buffer_info->format == xe::IndexFormat::kInt16 ? 2 : 4));
      draw.index_endian = index_buffer_info->endianness;
    }
    renderer_->OnDraw(*register_file_, vs_, ps_, draw);
    return true;
  }

  // The pipeline warm-up's progress, for the "Compiling shaders" panel (shader_progress.h; B-098).
  void PublishWarmup() {
    shader_precompile_total_.store(uint32_t(renderer_->warmup_total()), std::memory_order_relaxed);
    shader_precompile_done_.store(uint32_t(renderer_->warmup_done()), std::memory_order_relaxed);
    if (InstrState* is = FindInstrState()) {
      is->warm_total.store(uint32_t(renderer_->port_warmup_total()), std::memory_order_relaxed);
      is->warm_done.store(uint32_t(renderer_->port_warmup_done()), std::memory_order_relaxed);
      if (!pipeline_cache_path_.empty()) is->warm_loaded.store(1, std::memory_order_relaxed);
    }
  }

  // Test instrumentation (instr_state.h): the renderer's counters, once per swap.
  void PublishInstr(InstrState& is, uint32_t w, uint32_t h) {
    constexpr auto r = std::memory_order_relaxed;
    const auto& st = renderer_->stats();
    is.draws.store(st.draws, r), is.presents.store(st.presents, r), is.pending_draws.store(st.pending_draws, r);
    is.live_pipelines.store(st.live_pipelines, r), is.failed_pipeline_draws.store(st.failed_pipeline_draws, r);
    is.render_w.store(w, r), is.render_h.store(h, r);
  }

  // While the panel is in the foreground (the player is waiting on it, not playing) the guest stays at this swap
  // and the warm-up gets the whole CPU: on a 4-core laptop the game and a background warm-up starve each other
  // (B-098). Ends when the warm-up does or the player sends the panel to the background.
  struct FrameMark {
    std::chrono::steady_clock::time_point time;
    vk::Renderer::Stats stats;
    uint64_t idle_us = 0, query_idle_us = 0, sync_requests = 0, sync_locked = 0, sync_uploaded = 0, write_faults = 0;
  };

  FrameMark MarkFrame() const {
    FrameMark m{std::chrono::steady_clock::now(), renderer_->stats(), idle_us_, query_idle_us_};
    if (sync_) {
      m.sync_requests = sync_->requests(), m.sync_locked = sync_->locked(), m.sync_uploaded = sync_->uploaded();
      m.write_faults = sync_->invalidations();
    }
    return m;
  }

  void TrackFrame() {
    static const double hitch_ms = [] {
      const char* v = std::getenv("BOTS_HITCH_MS");
      return v && *v ? std::atof(v) : 25.0;
    }();
    FrameMark now = MarkFrame();
    const std::string load_max_what = renderer_->texture_load_max_what();  // before the take: read with it
    const uint64_t load_max_us = renderer_->TakeTextureLoadMaxUs();
    const vk::Renderer::N1FrameNote n1 = renderer_->TakeN1FrameNote();
    if (n1.held) frame_marked_ = false, frame_avg_ms_ = 0;  // the level preload hold: not playable yet, not a hitch
    if (frame_marked_) {
      const double ms = std::chrono::duration<double, std::milli>(now.time - frame_mark_.time).count();
      if (InstrState* is = FindInstrState()) is->Frame(ms);
      LogN1Hitch(ms, now, n1);
      static constexpr double kEdges[] = {20, 25, 33.4, 50, 100, 250};
      size_t bucket = 0;
      while (bucket < std::size(kEdges) && ms > kEdges[bucket]) ++bucket;
      ++frame_hist_[bucket];
      frame_max_ms_ = std::max(frame_max_ms_, ms);
      // Slow against the recent frames too: a scene that runs at a steady 30 fps (videos, some menus) isn't hitching.
      if (hitch_ms > 0 && ms > hitch_ms && ms > frame_avg_ms_ * 1.5) {
        if (hitch_lines_ < kHitchLines) {
          ++hitch_lines_;
          LogHitch(ms, now, load_max_us, load_max_what);
        } else {
          ++hitches_unlogged_;
        }
      }
      frame_avg_ms_ = frame_avg_ms_ ? frame_avg_ms_ * 0.9 + std::min(ms, 100.0) * 0.1 : ms;
    }
    frame_mark_ = std::move(now);
    frame_marked_ = true;
  }

  // B-199: every frame over BOTS_N1_HITCH_MS (33.4) within BOTS_N1_HITCH_WINDOW_S (60; 0: all run) of n1 resuming
  // after a gap (level start, end of a loading screen), with its likely cause. No relative test: at a level start
  // every slow frame counts. At most 2000 lines.
  void LogN1Hitch(double ms, const FrameMark& now, const vk::Renderer::N1FrameNote& n1) {
    static const double limit = [] {
      const char* v = std::getenv("BOTS_N1_HITCH_MS");
      return v && *v ? std::atof(v) : 33.4;
    }();
    static const double window = [] {
      const char* v = std::getenv("BOTS_N1_HITCH_WINDOW_S");
      return v && *v ? std::atof(v) : 60.0;
    }();
    const double since = renderer_->n1_seconds_since_resume();
    if (limit <= 0 || ms <= limit || since < 0 || (window > 0 && since > window) || n1_hitch_lines_ >= 2000) return;
    ++n1_hitch_lines_;
    const auto& a = frame_mark_.stats;
    const auto& b = now.stats;
    auto t = [](uint64_t from, uint64_t to) { return double(to - from) / 1000.0; };
    const double n1_load = n1.apply_ms + n1.upload_ms;
    const double port_tex = t(a.texture_us, b.texture_us), gpu_wait = t(a.wait_us, b.wait_us);
    const double idle = t(frame_mark_.idle_us, now.idle_us);
    const char* cause = "other";
    if (n1.tree_capture) {
      cause = "tree capture";
    } else if (n1_load >= 8 || (n1.applied && n1_load >= 0.25 * ms)) {
      cause = n1.lazy ? "n1 load (lazy)" : "n1 load (preloaded)";
    } else if (n1.late_pipelines || n1.late_shaders || n1.draws_no_pipe || b.pending_draws != a.pending_draws ||
               b.live_pipelines != a.live_pipelines) {
      cause = "pipeline";
    } else if (port_tex >= 8) {
      cause = "port texture upload";
    } else if (n1.record_ms >= 0.5 * ms) {
      cause = "n1 record";
    } else if (gpu_wait >= 0.4 * ms) {
      cause = "gpu";
    } else if (idle >= 0.5 * ms) {
      cause = "game thread";
    }
    REXLOG_INFO("n1 hitch: t+{:.1f} s frame {} took {:.1f} ms, cause {}: n1 record {:.1f} ms ({} scenes), loads {:.1f} "
                "ms ({} applied, longest {:.1f} ms, {} textures, {} meshes, {:.2f} MB; {} lazy, {} late skips), "
                "upload pump {:.1f} ms; late pipelines {} shaders {}, {} draws without a pipeline, {} on the stand-in; "
                "tree capture {}; port: draws {:.1f} ms, textures {:.1f} ms, GPU wait {:.1f} ms, CP idle {:.1f} ms, "
                "{} new port pipelines, {} capture draws not drawn",
                since, frames_, ms, cause, n1.record_ms, n1.scenes, n1.apply_ms, n1.applied, n1.apply_max_ms,
                n1.textures, n1.meshes, double(n1.bytes) / 1048576.0, n1.lazy, n1.late_skips, n1.upload_ms,
                n1.late_pipelines, n1.late_shaders, n1.draws_no_pipe, n1.fallback_draws, n1.tree_capture ? 1 : 0,
                t(a.draw_us, b.draw_us), port_tex, gpu_wait, idle, b.live_pipelines - a.live_pipelines,
                b.capture_skipped - a.capture_skipped);
  }

  // One slow frame: where this thread's time went (draw recording, of it texture loads, GPU fence waits, idle
  // waiting for the guest's commands; the rest is PM4 parsing, present and the frame limiter) and what it did.
  void LogHitch(double ms, const FrameMark& now, uint64_t load_max_us, const std::string& load_max_what) const {
    const auto& a = frame_mark_.stats;
    const auto& b = now.stats;
    auto t = [](uint64_t from, uint64_t to) { return double(to - from) / 1000.0; };
    const uint64_t loads = b.texture_loads - a.texture_loads;
    const uint64_t evicted = b.texture_loads_evicted - a.texture_loads_evicted;
    REXLOG_INFO(
        "gfx hitch: frame {} took {:.1f} ms: draws {:.1f} ms (textures {:.1f} ms: {} new, {} evicted earlier, {} "
        "rewritten, longest {:.1f} ms{}), GPU wait {:.1f} ms, CP idle {:.1f} ms ({:.1f} after queries); {} draws, {} "
        "flushes, {:.2f} MB uploaded, {} ranges invalidated, {} write faults, {} memory syncs ({} locked, {} "
        "uploaded), {} draws waiting on shaders or pipelines, {} new pipelines started, {} draws dropped for failed "
        "pipelines",
        frames_, ms, t(a.draw_us, b.draw_us), t(a.texture_us, b.texture_us), loads - evicted, evicted,
        b.texture_reloads - a.texture_reloads, load_max_us / 1000.0,
        loads + b.texture_reloads - a.texture_reloads ? fmt::format(": {}", load_max_what) : std::string(),
        t(a.wait_us, b.wait_us),
        t(frame_mark_.idle_us, now.idle_us), t(frame_mark_.query_idle_us, now.query_idle_us), b.draws - a.draws,
        b.flushes - a.flushes, double(b.uploaded_bytes - a.uploaded_bytes) / 1048576.0,
        b.invalidations - a.invalidations, now.write_faults - frame_mark_.write_faults,
        now.sync_requests - frame_mark_.sync_requests, now.sync_locked - frame_mark_.sync_locked,
        now.sync_uploaded - frame_mark_.sync_uploaded, b.pending_draws - a.pending_draws,
        b.live_pipelines - a.live_pipelines, b.failed_pipeline_draws - a.failed_pipeline_draws);
  }

  void ReportFrameTimes(uint64_t frames) {
    uint32_t over[7] = {};  // frames above each edge: over[i] = frames in buckets > i
    for (int i = 5; i >= 0; --i) over[i] = over[i + 1] + frame_hist_[i + 1];
    REXLOG_INFO("gfx native: frame times over {} frames: max {:.0f} ms; >20 ms {}, >25 ms {}, >33 ms {}, >50 ms {}, "
                ">100 ms {}, >250 ms {}{}",
                frames, frame_max_ms_, over[0], over[1], over[2], over[3], over[4], over[5],
                hitches_unlogged_ ? fmt::format("; {} slow frames not itemised", hitches_unlogged_) : std::string());
    std::fill(std::begin(frame_hist_), std::end(frame_hist_), 0);
    frame_max_ms_ = 0;
    hitch_lines_ = 0, hitches_unlogged_ = 0;
  }

  // The host report runs on its own thread: on one player's PC (issue #29) it took ~230 ms every time, a dropped frame
  // every 600 when it ran here. A report still running when the next is due skips that one.
  // Once per session unless diagnostic logging is on (BOTS_DIAG=1; the launcher's setting, 1.5.0).
  void RequestHostReport(std::chrono::steady_clock::time_point now) {
    static const bool periodic = [] {
      const char* v = std::getenv("BOTS_DIAG");
      return v && *v && std::strcmp(v, "0") != 0;
    }();
    std::lock_guard lock(host_mutex_);
    if (host_requested_ && !periodic) return;
    host_requested_ = true;
    if (host_quit_) return;
    if (host_pending_) {
      ++host_skipped_;
      return;
    }
    if (!host_thread_.joinable()) host_thread_ = std::thread([this] { HostReportLoop(); });
    host_now_ = now, host_pending_ = true;
    host_cv_.notify_one();
  }

  void HostReportLoop() {
    std::unique_lock lock(host_mutex_);
    for (;;) {
      host_cv_.wait(lock, [this] { return host_quit_ || host_pending_; });
      if (host_quit_) return;
      const auto now = host_now_;
      lock.unlock();
      ReportHost(now);
      lock.lock();
      host_pending_ = false;
    }
  }

  void StopHostReports() {
    {
      std::lock_guard lock(host_mutex_);
      host_quit_ = true;
    }
    host_cv_.notify_one();
    if (host_thread_.joinable()) host_thread_.join();
    std::lock_guard lock(host_mutex_);  // a later SetupContext starts a fresh thread
    host_quit_ = false, host_pending_ = false;
  }

  // CPU time of the whole process (all threads, in % of one core), clocks, a fixed-work probe, power, overlays.
  // Host report thread only.
  void ReportHost(std::chrono::steady_clock::time_point now) {
    auto ms_since = [](std::chrono::steady_clock::time_point t) {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    const auto start = std::chrono::steady_clock::now();
    const host::CpuTimes cpu = host::ProcessCpuTimes();
    const host::CpuClock clock = host::CpuClockMhz();
    const double probe = host::CpuProbeMicros();
    if (last_cpu_time_.time_since_epoch().count()) {
      const double wall_us = std::chrono::duration<double, std::micro>(now - last_cpu_time_).count();
      REXLOG_INFO("gfx native: host: process CPU {:.0f}% user + {:.0f}% kernel (of one core, {} cores), CPU clock "
                  "{} MHz of {} (limit {}), probe {:.0f} us, memory {:.0f} MB",
                  double(cpu.user_us - last_cpu_.user_us) / wall_us * 100,
                  double(cpu.kernel_us - last_cpu_.kernel_us) / wall_us * 100, std::thread::hardware_concurrency(),
                  clock.current_mhz, clock.max_mhz, clock.limit_mhz, probe, host::ProcessMemoryMB());
    }
    last_cpu_ = cpu, last_cpu_time_ = now;
    // Overlays once a minute: listing the process's modules is the slow part (/proc/self/maps on Linux).
    const double cpu_ms = ms_since(start);
    const auto power_start = std::chrono::steady_clock::now();
    std::string power = host::PowerState();
    const double power_ms = ms_since(power_start);
    const auto modules_start = std::chrono::steady_clock::now();
    std::string overlays = host_reports_++ % 6 ? last_overlays_ : host::OverlayModules();
    const double modules_ms = ms_since(modules_start);
    if (power != last_power_) REXLOG_INFO("gfx native: host power: {}", power.empty() ? "unknown" : power);
    if (overlays != last_overlays_) {
      REXLOG_INFO("gfx native: overlay modules: {}", overlays.empty() ? "none" : overlays);
    }
    last_power_ = std::move(power), last_overlays_ = std::move(overlays);
    uint32_t skipped;
    {
      std::lock_guard lock(host_mutex_);
      skipped = std::exchange(host_skipped_, 0);
    }
    REXLOG_INFO("gfx native: host report took {:.1f} ms (CPU and memory {:.1f}, power {:.1f}, modules {:.1f}; off the "
                "render thread){}",
                ms_since(start), cpu_ms, power_ms, modules_ms,
                skipped ? fmt::format("; {} reports skipped while this one ran", skipped) : std::string());
  }

  void HoldForWarmup() {
    if (!shader_precompile_foreground_.load(std::memory_order_relaxed) ||
        renderer_->warmup_done() >= renderer_->warmup_total()) {
      return;
    }
    const auto start = std::chrono::steady_clock::now();
    auto report = start + std::chrono::seconds(5);
    const size_t from = renderer_->warmup_done();
    InstrState* instr = FindInstrState();
    if (instr) instr->warm_hold.store(1, std::memory_order_relaxed);
    while (shader_precompile_foreground_.load(std::memory_order_relaxed) && worker_running_ &&
           renderer_->warmup_done() < renderer_->warmup_total()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      renderer_->PumpWarmup();
      PublishWarmup();
      if (std::chrono::steady_clock::now() >= report) {
        report += std::chrono::seconds(5);
        REXLOG_INFO("gfx native: guest held for the pipeline warm-up: {}/{} done", renderer_->warmup_done(),
                    renderer_->warmup_total());
      }
    }
    frame_marked_ = false, frame_avg_ms_ = 0;  // not a hitch: the next frame interval starts after the hold
    if (instr) instr->warm_hold.store(0, std::memory_order_relaxed);
    REXLOG_INFO("gfx native: guest held {:.1f} s for the pipeline warm-up ({} pipelines, {}/{} done)",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                // n1's share leaves warmup_done() when its startup step ends, so it can drop below `from`
                std::max(renderer_->warmup_done(), from) - from, renderer_->warmup_done(), renderer_->warmup_total());
  }

  void WriteRegister(uint32_t index, uint32_t value) override {
    if (index == bots::kHybridMarkerRegister) {  // hybrid spike: the game's scene was skipped here (hybrid_state.h)
      if (value == bots::kHybridCaptureBegin) {
        if (renderer_) renderer_->BeginCaptureFrame();
        return;
      }
      if (bots::HybridState* state = bots::FindHybridState()) {
        state->markers_seen.fetch_add(1, std::memory_order_relaxed);
        if (renderer_) renderer_->QueueSceneFill(value);
      }
      return;
    }
    CommandProcessor::WriteRegister(index, value);
    constant_writes_.Note(index);
  }

  // Shader constant uploads (SET_CONSTANT, LOAD_ALU_CONSTANT) are most of the register writes. Nothing in
  // WriteRegister reacts to a constant register, so a range entirely inside them is stored in one pass.
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override {
    if (start_index < 0x4000 || start_index + num_registers > 0x4928) {
      CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
      return;
    }
    rex::memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (num_registers) constant_writes_.NoteRange(start_index, num_registers);
  }

  // Copies arrive as draws in EDRAM copy mode, which the renderer resolves itself.
  bool IssueCopy() override { return true; }

  // Occlusion queries (renderer.h; BOTS_GFX_OCCLUSION=0/1). The SDK reports every query visible, so the game's
  // occlusion culling culls nothing. D3D marks a query's end by writing 0xFFFFFEED into the sample-count struct
  // before the event; a begin has no marker and starts from zero.
  bool ExecutePacketType3_EVENT_WRITE_ZPD(rex::memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override {
    if (!renderer_ || !renderer_->occlusion_queries()) {
      return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
    }
    const uint32_t initiator = reader->ReadAndSwap<uint32_t>();
    WriteRegister(rg::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
    const uint32_t address = register_file_->values[rg::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
    auto* counts = memory_->TranslatePhysical<xe::xe_gpu_depth_sample_counts*>(address);
    if (!counts) return true;
    const uint32_t marker = rex::byte_swap(0xFFFFFEEDu);
    const bool end = counts->ZPass_A == marker || counts->ZPass_B == marker || counts->ZFail_A == marker ||
                     counts->ZFail_B == marker;
    if (end) {
      renderer_->EndOcclusionQuery(address);
    } else {
      std::memset(counts, 0, sizeof(*counts));
      renderer_->BeginOcclusionQuery();
    }
    return true;
  }

  // The command stream ran dry (or the CP waits on memory): the guest may be spinning on a query result, so submit
  // the recorded queries now rather than at the frame's end. UE3 waits (GetQueryResult, bWait = 1) on queries of the
  // frame it is drawing, and its GetData spin kicks the ring every iteration, so holding back until the stream has
  // stayed dry for a while never fires (tried: Deck 64 -> 39.5 fps).
  void PrepareForWait() override {
    query_wait_ = renderer_ && renderer_->occlusion_unsubmitted();
    if (query_wait_) renderer_->Flush();
    wait_start_ = std::chrono::steady_clock::now();
    CommandProcessor::PrepareForWait();
  }
  // Profiling: time the CP spent with nothing to do, and the part of it that began with queries just submitted.
  void ReturnFromWait() override {
    const auto us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start_).count());
    idle_us_ += us;
    if (query_wait_) query_idle_us_ += us;
    CommandProcessor::ReturnFromWait();
  }

 private:
  std::unique_ptr<vk::Renderer> renderer_;
  std::unique_ptr<vk::MemorySync> sync_;
  std::filesystem::path pipeline_cache_path_;
  std::string n1_cache_dir_;  // <cache root>/n1: the new renderer's asset cache (hybrid_fill.cpp)
  std::string n1_cooked_dir() const {
    return bots::PathUtf8(kernel_state_->emulator()->game_data_root() / "C1Game" / "CookedXenon");
  }
  ShaderLoad vs_, ps_;
  std::vector<uint32_t> vs_inline_, ps_inline_;
  ConstantWriteTracker constant_writes_;
  uint64_t frames_ = 0, indirect_draws_ = 0;
  struct {
    uint64_t frames = 0;
    std::chrono::steady_clock::time_point time;
    uint64_t draw_us = 0, texture_us = 0, wait_us = 0, flushes = 0, draws = 0, resolves = 0;
    uint64_t render_passes = 0, transfers = 0, barriers = 0, pending_draws = 0, gpu_ns[7] = {}, draw_phase_ns[12] = {};
    uint64_t index_reuses = 0, index_converts = 0, invalidations = 0;
    uint64_t texture_loads = 0, texture_loads_evicted = 0, texture_reloads = 0, texture_evictions = 0;
  } last_;
  // Frame-time diagnostics (issue #12): the interval between swaps on this thread, what each slow one did (a "gfx
  // hitch" line, at most kHitchLines per stats interval), and a histogram per interval. BOTS_HITCH_MS sets the
  // threshold (default 25 ms; 0 turns the lines off).
  static constexpr uint32_t kHitchLines = 40;
  FrameMark frame_mark_;
  bool frame_marked_ = false;
  uint32_t frame_hist_[7] = {};  // <=20, 20-25, 25-33, 33-50, 50-100, 100-250, >250 ms
  double frame_max_ms_ = 0, frame_avg_ms_ = 0;  // frame_avg_ms_: moving average, slow frames capped at 100 ms
  uint32_t hitch_lines_ = 0, hitches_unlogged_ = 0, n1_hitch_lines_ = 0;
  host::CpuTimes last_cpu_;
  std::chrono::steady_clock::time_point last_cpu_time_;
  std::string last_power_ = "-", last_overlays_ = "-";  // "-": not reported yet
  uint32_t host_reports_ = 0;
  std::thread host_thread_;
  std::mutex host_mutex_;
  std::condition_variable host_cv_;
  std::chrono::steady_clock::time_point host_now_;  // host_mutex_ guards these five
  bool host_pending_ = false, host_quit_ = false, host_requested_ = false;
  uint32_t host_skipped_ = 0;
  uint32_t indirect_depth_ = 0;
  bool query_wait_ = false;
  std::chrono::steady_clock::time_point wait_start_;
  uint64_t idle_us_ = 0, query_idle_us_ = 0, last_idle_us_ = 0, last_query_idle_us_ = 0;
};

class NativeGraphicsSystem final : public rg::GraphicsSystem {
 public:
  std::string name() const override { return "Native"; }

 protected:
  void CreateProvider(bool with_presentation) override {
    provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
  }

 private:
  std::unique_ptr<rg::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace
}  // namespace bots::gfx

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion || !info ||
      info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-native: incompatible host (ABI {})", abi_version);
    return nullptr;
  }
  return new bots::gfx::NativeGraphicsSystem();
}
