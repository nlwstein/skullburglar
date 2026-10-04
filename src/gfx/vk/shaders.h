// Guest shader microcode -> SPIR-V, through the SDK's Xenos translator, and the worker threads that translate
// shaders and create pipelines off the GPU thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/string/buffer.h>

namespace bots::gfx::vk {

// Progress of an asynchronous result (a translation, a pipeline). Written by a worker with release, read with
// acquire: the result's data is published by the store of kReady/kFailed.
enum class Async : uint8_t { kNone, kPending, kReady, kFailed };

// Host pixels per guest pixel (BOTS_GFX_RENDER_SCALE, 0.25-1; 1 = native resolution): surfaces and resolve
// destinations are allocated, drawn and copied at this fraction of the guest's size. Unset: 2/3 on Intel GPUs, else 1.
// Decided on the first call (the renderer makes it before translating anything).
float RenderScale(const rex::ui::vulkan::VulkanDevice* device);

// Worker threads, each with its own translator (translators are not thread-safe). Jobs run in submission order.
// `count` workers take live work first, then background work; `background` more run background work only, at low OS
// priority, so a cold warm-up can use the idle cores without taking CPU from the game.
class Workers {
 public:
  using Job = std::function<void(rex::graphics::SpirvShaderTranslator&)>;
  Workers(const rex::ui::vulkan::VulkanDevice* device, uint32_t count, uint32_t background = 0);
  ~Workers();  // finishes the queue's running jobs, drops the rest
  uint32_t count() const { return uint32_t(threads_.size()); }
  // Background jobs (pipeline warm-up) run only when no other job is queued. First jobs (live shader translations:
  // short, CPU only) run before everything else, on any worker, so a draw never waits for its translation behind
  // pipeline compiles (slow on a cold Metal cache).
  void Submit(Job job, bool background = false, bool first = false);
  // Blocks until `state` is no longer kPending, running queued jobs on this thread (with `translator`) meanwhile, so
  // a waiting GPU thread is never stuck behind a long queue.
  void Wait(const std::atomic<Async>& state, rex::graphics::SpirvShaderTranslator& translator);
  size_t queued() const;
  size_t background_queued() const;

 private:
  void Run(const rex::ui::vulkan::VulkanDevice* device, bool background_only);
  mutable std::mutex mutex_;
  std::condition_variable work_, done_;
  std::deque<Job> first_, queue_, background_;
  bool Take(Job& job);  // under mutex_: the next job, live work first
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

class ShaderCache {
 public:
  using Shader = rex::graphics::vulkan::VulkanShader;
  using Translation = Shader::VulkanTranslation;

  // workers: 0 translates on the calling thread only (Request's sync is then always true in effect).
  ShaderCache(const rex::ui::vulkan::VulkanDevice* device, uint32_t workers, uint32_t background_workers = 0);
  ~ShaderCache();

  // The shader whose big-endian microcode is at `ucode`, analyzed. Cached by content hash.
  Shader* Get(rex::graphics::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords);
  // The shader's translation for `modification`, translated and with a module once kReady. Starts the translation
  // on a worker if it hasn't been; sync waits for it (kReady or kFailed then).
  Async Request(Shader& shader, uint64_t modification, Translation*& translation, bool sync,
                bool background = false);

  const rex::graphics::SpirvShaderTranslator& translator() const { return translator_; }
  // Joins the workers (dropping queued jobs) before what their jobs use goes away.
  void StopWorkers() { workers_.reset(); }
  // For the renderer's pipeline jobs.
  Workers* workers() { return workers_.get(); }
  void Wait(const std::atomic<Async>& state) {
    if (workers_) workers_->Wait(state, translator_);
  }

 private:
  static bool Translate(rex::graphics::SpirvShaderTranslator& translator, Translation& translation);
  const rex::ui::vulkan::VulkanDevice* device_;
  rex::graphics::SpirvShaderTranslator translator_;  // the GPU thread's
  rex::string::StringBuffer disasm_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
  std::unordered_map<const Translation*, std::unique_ptr<std::atomic<Async>>> states_;
  // One translation of a shader at a time: the first one also fills the shader's binding lists, which other
  // translations of it then skip, and draws read them once theirs is ready.
  std::unordered_map<const Shader*, const std::atomic<Async>*> translating_;
  std::unique_ptr<Workers> workers_;  // last member: joined first
};

}  // namespace bots::gfx::vk
