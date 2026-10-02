// The native renderer as a GPU plugin (librexgpu-native, selected with REX_GPU_PLUGIN=native or --gpu_plugin
// native). The SDK's command processor still consumes the runtime's PM4 ring on its own thread, and with it the
// CPU/GPU handshakes the runtime relies on (scratch-register writeback, interrupts to the guest's callback,
// WAIT_REG_MEM, coherency, gamma-ramp registers, occlusion-query events); draws, resolves and presents go to the
// native renderer instead of the emulated EDRAM backend. A plugin rather than part of the game so its SDK sources
// and their cvars live in their own module, as the xenos plugin's do.
#include <algorithm>
#include <chrono>
#include <string_view>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/registers.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include "pm4.h"
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
    renderer_->Flush();
    renderer_->EndFrame();
    ++frames_;
    rex::ui::Presenter* presenter = graphics_system_->presenter();
    if (presenter) {
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      const uint32_t width = frontbuffer_width ? frontbuffer_width : uint32_t(video_mode.display_width);
      const uint32_t height = frontbuffer_height ? frontbuffer_height : uint32_t(video_mode.display_height);
      const auto* ramp = reinterpret_cast<const uint32_t*>(gamma_ramp_256_entry_table());
      presenter->RefreshGuestOutput(
          width, height, std::max(1u, uint32_t(video_mode.display_width)),
          std::max(1u, uint32_t(video_mode.display_height)),
          [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& vulkan_context =
                static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
            context.SetIs8bpc(true);
            return renderer_->Present(frontbuffer_ptr & 0x1FFFFFFF, vulkan_context.image(),
                                      vulkan_context.image_view(), width, height, ramp);
          });
    }
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
            "GPU wait, {:.1f} flushes, {:.0f} draws, {:.1f} resolves",
            frames_ - last_.frames,
            std::chrono::duration<double, std::milli>(now - last_.time).count() / n, ms(st.draw_us, last_.draw_us),
            ms(st.texture_us, last_.texture_us), ms(st.wait_us, last_.wait_us), double(st.flushes - last_.flushes) / n,
            double(st.draws - last_.draws) / n, double(st.resolves - last_.resolves) / n);
      }
      last_ = {frames_, now, st.draw_us, st.texture_us, st.wait_us, st.flushes, st.draws, st.resolves};
      REXLOG_INFO(
          "gfx native: frame {}: {} draws, {} resolves, {} presents, {} failed shaders, {} unsupported prims, "
          "{} unsupported textures, {:.1f} MB uploaded, {} invalidations, {} texture reloads, {} evictions, {} indirect draws skipped (totals)",
          frames_, st.draws, st.resolves, st.presents, st.failed_shaders, st.unsupported_primitives,
          st.unsupported_textures, st.uploaded_bytes / 1048576.0, st.invalidations, st.texture_reloads,
          st.texture_evictions, indirect_draws_);
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
    REXLOG_INFO("gfx native: renderer up");
    return true;
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

  // Copies arrive as draws in EDRAM copy mode, which the renderer resolves itself.
  bool IssueCopy() override { return true; }

 private:
  std::unique_ptr<vk::Renderer> renderer_;
  std::unique_ptr<vk::MemorySync> sync_;
  ShaderLoad vs_, ps_;
  std::vector<uint32_t> vs_inline_, ps_inline_;
  uint64_t frames_ = 0, indirect_draws_ = 0;
  struct {
    uint64_t frames = 0;
    std::chrono::steady_clock::time_point time;
    uint64_t draw_us = 0, texture_us = 0, wait_us = 0, flushes = 0, draws = 0, resolves = 0;
  } last_;
  uint32_t indirect_depth_ = 0;
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
