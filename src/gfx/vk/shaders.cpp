#include "shaders.h"

#include <cstdio>

#include <xxhash.h>

namespace bots::gfx::vk {

namespace rg = rex::graphics;

namespace {

// Host render targets: no fragment shader interlock, no 2x MSAA tricks (the game renders without MSAA).
#define BOTS_TRANSLATOR_ARGS(device) rg::SpirvShaderTranslator::Features(device), false, false, false

}  // namespace

Workers::Workers(const rex::ui::vulkan::VulkanDevice* device, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) threads_.emplace_back([this, device] { Run(device); });
}

Workers::~Workers() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    queue_.clear();
  }
  work_.notify_all();
  for (std::thread& thread : threads_) thread.join();
}

void Workers::Submit(Job job) {
  {
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(job));
  }
  work_.notify_one();
}

size_t Workers::queued() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

void Workers::Run(const rex::ui::vulkan::VulkanDevice* device) {
  rg::SpirvShaderTranslator translator(BOTS_TRANSLATOR_ARGS(device));
  std::unique_lock lock(mutex_);
  for (;;) {
    work_.wait(lock, [this] { return stop_ || !queue_.empty(); });
    if (stop_) return;
    Job job = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    job(translator);
    lock.lock();
    done_.notify_all();
  }
}

void Workers::Wait(const std::atomic<Async>& state, rg::SpirvShaderTranslator& translator) {
  std::unique_lock lock(mutex_);
  while (state.load(std::memory_order_acquire) == Async::kPending) {
    if (!queue_.empty()) {
      Job job = std::move(queue_.front());
      queue_.pop_front();
      lock.unlock();
      job(translator);
      lock.lock();
      done_.notify_all();
    } else {
      done_.wait(lock);
    }
  }
}

ShaderCache::ShaderCache(const rex::ui::vulkan::VulkanDevice* device, uint32_t workers)
    : device_(device), translator_(BOTS_TRANSLATOR_ARGS(device)) {
  if (workers) workers_ = std::make_unique<Workers>(device, workers);
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
  }
  return translation.is_valid() && translation.GetOrCreateShaderModule();
}

Async ShaderCache::Request(Shader& shader, uint64_t modification, Translation*& translation, bool sync) {
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
    workers_->Submit([this, translation, done = state.get()](rg::SpirvShaderTranslator& translator) {
      done->store(Translate(translator, *translation) ? Async::kReady : Async::kFailed, std::memory_order_release);
    });
    return Async::kPending;
  }
  if (now == Async::kPending && sync) {
    Wait(*state);
    now = state->load(std::memory_order_acquire);
  }
  return now;
}

}  // namespace bots::gfx::vk
