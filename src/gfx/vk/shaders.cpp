#include "shaders.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace bots::gfx::vk {

namespace rg = rex::graphics;

namespace {

bool EnvFlag(const char* name, bool fallback) {
  const char* env = std::getenv(name);
  return env && *env ? *env != '0' : fallback;
}

// Let the driver fuse float ops: the translator otherwise marks every op NoContraction, which the Metal path turns
// into an unoptimizable helper call per op (several times the GPU time). Pixel shaders don't need exactness.
// Vertex shaders rely on the Invariant position output for the depth prepass / EQUAL-test passes to line up; Metal
// honours it natively, so contraction is on there only. BOTS_GFX_PS_CONTRACTION / BOTS_GFX_VS_CONTRACTION=0|1.
rg::SpirvShaderTranslator::Features TranslatorFeatures(const rex::ui::vulkan::VulkanDevice* device) {
  rg::SpirvShaderTranslator::Features features(device);
  features.render_scale = RenderScale(device);
  features.fragment_contraction = EnvFlag("BOTS_GFX_PS_CONTRACTION", true);
#ifdef __APPLE__
  features.vertex_contraction = EnvFlag("BOTS_GFX_VS_CONTRACTION", true);
#else
  features.vertex_contraction = EnvFlag("BOTS_GFX_VS_CONTRACTION", false);
#endif
  return features;
}

// For background-only workers: below the game's threads, so they use idle cores only.
void LowerThreadPriority() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
  setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 10);  // Linux nice is per thread
#endif
}

// Host render targets: no fragment shader interlock, no 2x MSAA tricks (the game renders without MSAA).
#define BOTS_TRANSLATOR_ARGS(device) TranslatorFeatures(device), false, false, false

}  // namespace

float RenderScale(const rex::ui::vulkan::VulkanDevice* device) {
  static const float scale = [device] {
    // BOTS_GFX_RENDER_SCALE, then the render_scale setting (bots.toml / launcher; "auto" or a fraction).
    std::string v;
    if (const char* env = std::getenv("BOTS_GFX_RENDER_SCALE"); env && *env) {
      v = env;
    } else {
      v = rex::cvar::GetFlagByName("render_scale");
    }
    if (!v.empty() && v != "auto") {
      const float s = float(std::atof(v.c_str()));
      return s >= 0.25f && s < 1.0f ? s : 1.0f;
    }
    // Intel integrated GPUs are fill-bound at 720p (a UHD 620 draws a frame in ~50 ms at 1, ~28 ms at 2/3).
    return device && device->properties().vendorID == 0x8086 ? 2.0f / 3.0f : 1.0f;
  }();
  return scale;
}

Workers::Workers(const rex::ui::vulkan::VulkanDevice* device, uint32_t count, uint32_t background) {
  for (uint32_t i = 0; i < count + background; ++i)
    threads_.emplace_back([this, device, only = i >= count] { Run(device, only); });
}

Workers::~Workers() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    first_.clear();
    queue_.clear();
    background_.clear();
  }
  work_.notify_all();
  for (std::thread& thread : threads_) thread.join();
}

void Workers::Submit(Job job, bool background, bool first) {
  {
    std::lock_guard lock(mutex_);
    (first ? first_ : background ? background_ : queue_).push_back(std::move(job));
  }
  // Only a background job can go to any worker. notify_one for a live job can wake a background-only worker, which
  // sleeps again, and the job then waits for the next unrelated Submit: up to 22 s on the Deck (2026-10-04, live
  // pipelines queued with 0 running). Live and first jobs wake them all.
  if (background && !first) {
    work_.notify_one();
  } else {
    work_.notify_all();
  }
}

size_t Workers::queued() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

size_t Workers::background_queued() const {
  std::lock_guard lock(mutex_);
  return background_.size();
}

bool Workers::Take(Job& job) {
  std::deque<Job>& from = !first_.empty() ? first_ : !queue_.empty() ? queue_ : background_;
  if (from.empty()) return false;
  job = std::move(from.front());
  from.pop_front();
  return true;
}

void Workers::Run(const rex::ui::vulkan::VulkanDevice* device, bool background_only) {
  if (background_only) LowerThreadPriority();
  rg::SpirvShaderTranslator translator(BOTS_TRANSLATOR_ARGS(device));
  std::unique_lock lock(mutex_);
  for (;;) {
    work_.wait(lock, [this, background_only] {
      return stop_ || !first_.empty() || (!background_only && !queue_.empty()) || !background_.empty();
    });
    if (stop_) return;
    Job job;
    if (background_only) {
      std::deque<Job>& from = !first_.empty() ? first_ : background_;
      job = std::move(from.front());
      from.pop_front();
    } else {
      Take(job);
    }
    ++running_;
    lock.unlock();
    job(translator);
    lock.lock();
    --running_;
    done_.notify_all();
  }
}

std::string Workers::Describe() const {
  std::lock_guard lock(mutex_);
  return fmt::format("{}/{}/{} queued, {} running", first_.size(), queue_.size(), background_.size(), running_);
}

void Workers::Wait(const std::atomic<Async>& state, rg::SpirvShaderTranslator& translator) {
  std::unique_lock lock(mutex_);
  // What's awaited may itself be a queued background job, so those run here too (the warm-up keeps that queue short).
  while (state.load(std::memory_order_acquire) == Async::kPending) {
    if (Job job; Take(job)) {
      lock.unlock();
      job(translator);
      lock.lock();
      done_.notify_all();
    } else {
      done_.wait(lock);
    }
  }
}

ShaderCache::ShaderCache(const rex::ui::vulkan::VulkanDevice* device, uint32_t workers, uint32_t background_workers)
    : device_(device), translator_(BOTS_TRANSLATOR_ARGS(device)) {
  if (workers) workers_ = std::make_unique<Workers>(device, workers, background_workers);
}

ShaderCache::~ShaderCache() { workers_.reset(); }

ShaderCache::Shader* ShaderCache::Get(rg::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords) {
  // Vertex and pixel microcode can be identical; keep them apart.
  const uint64_t hash = XXH3_64bits(ucode, size_t(dwords) * 4) ^ uint64_t(type);
  auto& slot = shaders_[hash];
  if (!slot) {
    slot = std::make_unique<Shader>(device_, type, hash, ucode, dwords);
    slot->AnalyzeUcode(disasm_);
  }
  return slot.get();
}

bool ShaderCache::Translate(rg::SpirvShaderTranslator& translator, Translation& translation) {
  if (!translation.is_translated() && !translator.TranslateAnalyzedShader(translation)) {
    std::fprintf(stderr, "native renderer: shader %016llX translation failed\n",
                 (unsigned long long)translation.shader().ucode_data_hash());
  } else if (static const char* dir = std::getenv("BOTS_DUMP_SPIRV"); dir && *dir) {
    // BOTS_DUMP_SPIRV=<dir>: every translation as <dir>/<ucode hash>-<modification>.spv (inspect with spirv-dis /
    // spirv-cross; debugging).
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%016llX-%016llX.spv", dir,
                  (unsigned long long)translation.shader().ucode_data_hash(),
                  (unsigned long long)translation.modification());
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(translation.translated_binary().data(), 1, translation.translated_binary().size(), f);
      std::fclose(f);
    }
  }
  return translation.is_valid() && translation.GetOrCreateShaderModule();
}

Async ShaderCache::Request(Shader& shader, uint64_t modification, Translation*& translation, bool sync,
                           bool background) {
  // GetOrCreateTranslation is only ever called here, on the GPU thread; workers only fill in the translation.
  translation = static_cast<Translation*>(shader.GetOrCreateTranslation(modification));
  auto& state = states_[translation];
  if (!state) state = std::make_unique<std::atomic<Async>>(Async::kNone);
  Async now = state->load(std::memory_order_acquire);
  if (now == Async::kNone) {
    auto busy = translating_.find(&shader);
    if (busy != translating_.end() && busy->second->load(std::memory_order_acquire) == Async::kPending) {
      if (!sync) return Async::kPending;  // asked again by a later draw
      Wait(*busy->second);
    }
    if (!workers_ || sync) {
      now = Translate(translator_, *translation) ? Async::kReady : Async::kFailed;
      state->store(now, std::memory_order_release);
      return now;
    }
    state->store(Async::kPending, std::memory_order_relaxed);
    translating_[&shader] = state.get();
    workers_->Submit(
        [this, translation, done = state.get()](rg::SpirvShaderTranslator& translator) {
          done->store(Translate(translator, *translation) ? Async::kReady : Async::kFailed, std::memory_order_release);
        },
        background, !background);
    return Async::kPending;
  }
  if (now == Async::kPending && sync) {
    Wait(*state);
    now = state->load(std::memory_order_acquire);
  }
  return now;
}

}  // namespace bots::gfx::vk
