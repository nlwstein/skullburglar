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

// Worker threads, each with its own translator (translators are not thread-safe). Jobs run in submission order.
class Workers {
 public:
  using Job = std::function<void(rex::graphics::SpirvShaderTranslator&)>;
  Workers(const rex::ui::vulkan::VulkanDevice* device, uint32_t count);
  ~Workers();  // finishes the queue's running jobs, drops the rest
  uint32_t count() const { return uint32_t(threads_.size()); }
  void Submit(Job job);
  // Blocks until `state` is no longer kPending, running queued jobs on this thread (with `translator`) meanwhile, so
  // a waiting GPU thread is never stuck behind a long queue.
  void Wait(const std::atomic<Async>& state, rex::graphics::SpirvShaderTranslator& translator);
  size_t queued() const;

 private:
  void Run(const rex::ui::vulkan::VulkanDevice* device);
  mutable std::mutex mutex_;
  std::condition_variable work_, done_;
  std::deque<Job> queue_;
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

class ShaderCache {
 public:
  using Shader = rex::graphics::vulkan::VulkanShader;
  using Translation = Shader::VulkanTranslation;

  // workers: 0 translates on the calling thread only (Request's sync is then always true in effect).
  ShaderCache(const rex::ui::vulkan::VulkanDevice* device, uint32_t workers);
  ~ShaderCache();

  // The shader whose big-endian microcode is at `ucode`, analyzed. Cached by content hash.
  Shader* Get(rex::graphics::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords);
  // The shader's translation for `modification`, translated and with a module once kReady. Starts the translation
  // on a worker if it hasn't been; sync waits for it (kReady or kFailed then).
  Async Request(Shader& shader, uint64_t modification, Translation*& translation, bool sync);

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
