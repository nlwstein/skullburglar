// The native renderer as a GPU plugin (librexgpu-native, selected with REX_GPU_PLUGIN=native or --gpu_plugin
// native). The SDK's command processor still consumes the runtime's PM4 ring on its own thread, and with it the
// CPU/GPU handshakes the runtime relies on (scratch-register writeback, interrupts to the guest's callback,
// WAIT_REG_MEM, coherency, gamma-ramp registers, occlusion-query events); draws, resolves and presents go to the
// native renderer instead of the emulated EDRAM backend. A plugin rather than part of the game so its SDK sources
// and their cvars live in their own module, as the xenos plugin's do.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
#include <rex/ui/vulkan/provider.h>

#include "pm4.h"
#include "upk.h"
#include "vk/memory_sync.h"
#include "vk/renderer.h"

namespace bots::gfx {
namespace {

namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

class NativeCommandProcessor final : public rg::CommandProcessor {
 public:
  NativeCommandProcessor(rg::GraphicsSystem* graphics_system, rex::system::KernelState* kernel_state)
      : CommandProcessor(graphics_system, kernel_state) {}

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override {
    if (!renderer_) return;
    renderer_->EndFrame();
    PublishWarmup();
    HoldForWarmup();
    ++frames_;
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
      const auto* ramp = reinterpret_cast<const uint32_t*>(gamma_ramp_256_entry_table());
      presenter->RefreshGuestOutput(
          out_width, out_height, std::max(1u, uint32_t(video_mode.display_width)),
          std::max(1u, uint32_t(video_mode.display_height)),
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
      }
      REXLOG_INFO("gfx native: memory: {}", renderer_->TakeMemoryReport());
      last_idle_us_ = idle_us_, last_query_idle_us_ = query_idle_us_;
      last_ = {frames_, now, st.draw_us, st.texture_us, st.wait_us, st.flushes, st.draws, st.resolves};
      last_.render_passes = st.render_passes, last_.transfers = st.transfers, last_.barriers = st.barriers;
      last_.pending_draws = st.pending_draws;
      last_.index_reuses = st.index_reuses, last_.index_converts = st.index_converts;
      last_.invalidations = st.invalidations;
      renderer_->SavePipelineCache();  // runs are often killed rather than shut down
      std::copy(std::begin(st.gpu_ns), std::end(st.gpu_ns), last_.gpu_ns);
      std::copy(std::begin(st.draw_phase_ns), std::end(st.draw_phase_ns), last_.draw_phase_ns);
      REXLOG_INFO(
          "gfx native: frame {}: {} draws, {} resolves, {} presents, {} failed shaders, {} unsupported prims, "
          "{} unsupported textures, {:.1f} MB uploaded, {} invalidations, {} texture reloads, {} evictions, {} indirect draws skipped, "
          "{} draws skipped for pipelines, {}/{} pipelines warmed, {} occlusion queries ({} uncounted, {} hidden) "
          "(totals)",
          frames_, st.draws, st.resolves, st.presents, st.failed_shaders, st.unsupported_primitives,
          st.unsupported_textures, st.uploaded_bytes / 1048576.0, st.invalidations, st.texture_reloads,
          st.texture_evictions, indirect_draws_, st.pending_draws, renderer_->warmup_done(),
          renderer_->warmup_total(), st.occlusion_queries, st.occlusion_inexact, renderer_->occlusion_hidden());
    }
  }

 protected:
  bool SetupContext() override {
    auto* provider = static_cast<rex::ui::vulkan::VulkanProvider*>(graphics_system_->provider());
    if (!provider) return false;
    rex::memory::Memory* memory = memory_;
    renderer_ = std::make_unique<vk::Renderer>(provider->vulkan_device(), [memory](uint32_t address) {
      return memory->TranslatePhysical<const uint8_t*>(address);
    });
    if (!renderer_->ok()) {
      REXLOG_ERROR("gfx native: renderer initialisation failed");
      renderer_.reset();
      return false;
    }
    sync_ = std::make_unique<vk::MemorySync>(*memory_, *renderer_);
    renderer_->SetMemoryTracker(sync_.get());
    // Shipped shader refs (B-098): next to the executable in pipelines/, read from the player's disc and XEX image.
    auto package = std::make_shared<std::pair<std::string, std::unique_ptr<upk::Package>>>();
    renderer_->SetShaderRefs(
        {rex::filesystem::GetExecutableFolder() / "pipelines"},
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
    if (!pipeline_cache_path_.empty()) renderer_->LoadPipelineCache(pipeline_cache_path_);
    PublishWarmup();
    const char* occlusion = std::getenv("BOTS_GFX_OCCLUSION");
    if (occlusion && *occlusion ? *occlusion == '1' : renderer_->occlusion_queries_by_default()) {
      renderer_->EnableOcclusionQueries(1000);  // "visible": the SDK's fake count
    }
    REXLOG_INFO("gfx native: occlusion queries {}", renderer_->occlusion_queries() ? "on" : "off");
    REXLOG_INFO("gfx native: renderer up");
    return true;
  }

  // Runs on the command processor thread, or before the context is set up while the processor is paused.
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) override {
    pipeline_cache_path_ = cache_root / "native" / fmt::format("{:08X}.pipelines.bin", title_id);
    if (renderer_) {
      renderer_->LoadPipelineCache(pipeline_cache_path_);
      PublishWarmup();
    }
  }

  void ShutdownContext() override {
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
  }

  // While the panel is in the foreground (the player is waiting on it, not playing) the guest stays at this swap
  // and the warm-up gets the whole CPU: on a 4-core laptop the game and a background warm-up starve each other
  // (B-098). Ends when the warm-up does or the player sends the panel to the background.
  void HoldForWarmup() {
    if (!shader_precompile_foreground_.load(std::memory_order_relaxed) ||
        renderer_->warmup_done() >= renderer_->warmup_total()) {
      return;
    }
    const auto start = std::chrono::steady_clock::now();
    auto report = start + std::chrono::seconds(5);
    const size_t from = renderer_->warmup_done();
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
    REXLOG_INFO("gfx native: guest held {:.1f} s for the pipeline warm-up ({} pipelines, {}/{} done)",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                renderer_->warmup_done() - from, renderer_->warmup_done(), renderer_->warmup_total());
  }

  void WriteRegister(uint32_t index, uint32_t value) override {
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
  } last_;
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
