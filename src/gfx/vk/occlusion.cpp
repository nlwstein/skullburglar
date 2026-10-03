// Occlusion queries (renderer.h, "Occlusion queries"). The XDK's D3DQuery_GetData sums the ZPass counters of the
// query's sample-count blocks (with predicated tiling, one per tile: the replays end theirs inside nested indirect
// buffers, whose draws the renderer has already drawn once) and reports the query done once the last block's ZPass
// words no longer hold the end marker. So results are written in the order the queries ended: the resolver takes
// submissions in submit order and each one's queries in order, and a tile's zero block never lands before the
// first block's count. A torn read (one ZPass word new, the other still the marker) sums to a huge count: "visible".
#include <atomic>
#include <cstring>

#include <rex/graphics/xenos.h>

#include "renderer.h"

namespace bots::gfx::vk {

namespace xe = rex::graphics::xenos;

void Renderer::EnableOcclusionQueries(uint32_t visible_samples) {
  if (occlusion_pool_size_) return;
  Flush();  // the next recording begins by resetting the new pools
  constexpr uint32_t kSlots = 8192;  // per submission; ~450 queried draws per frame, a few submissions per frame
  const VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_OCCLUSION,
                                   kSlots};
  for (Submission& s : submissions_) {
    if (vk_.vkCreateQueryPool(device_->device(), &info, nullptr, &s.occlusion_pool) != VK_SUCCESS) return;
  }
  occlusion_visible_ = visible_samples;
  occlusion_flags_ = device_->properties().occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
  occlusion_pool_size_ = kSlots;  occlusion_thread_ = std::thread([this] { ResolveOcclusion(); });
}

bool Renderer::occlusion_queries_by_default() const {
  const VkDriverId driver = device_->properties().driverID;
  return driver != VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS && driver != VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA;
}

void Renderer::BeginOcclusionQuery() {
  if (!occlusion_pool_size_) return;
  occlusion_open_ = true;
  occlusion_inexact_ = false;
  occlusion_draws_ = occlusion_counted_ = 0;
  occlusion_flush_ = stats_.flushes;
  occlusion_first_ = submissions_[current_].occlusion_slots;
}

void Renderer::BeginDrawQuery() {
  if (!occlusion_open_ || occlusion_inexact_) return;
  Submission& s = submissions_[current_];
  if (occlusion_flush_ != stats_.flushes) {
    // Submitted since the query began: start over in this submission if nothing was counted yet.
    if (occlusion_counted_) {
      occlusion_inexact_ = true;
      return;
    }
    occlusion_flush_ = stats_.flushes;
    occlusion_first_ = s.occlusion_slots;
  }
  if (s.occlusion_slots >= occlusion_pool_size_) {
    occlusion_inexact_ = true;
    return;
  }
  vk_.vkCmdBeginQuery(cmd_, s.occlusion_pool, s.occlusion_slots, occlusion_flags_);
  occlusion_drawing_ = true;
}

void Renderer::EndDrawQuery() {
  if (!occlusion_drawing_) return;
  Submission& s = submissions_[current_];
  vk_.vkCmdEndQuery(cmd_, s.occlusion_pool, s.occlusion_slots++);
  occlusion_drawing_ = false;
  ++occlusion_counted_;
}

void Renderer::EndOcclusionQuery(uint32_t address) {
  if (!occlusion_pool_size_) return;
  ++stats_.occlusion_queries;
  Submission& s = submissions_[current_];
  // Exact when every draw that reached the renderer (OnDraw) was counted - none skipped for a missing pipeline,
  // unsupported state... - in the submission still being recorded.
  const bool exact = occlusion_open_ && !occlusion_inexact_ && occlusion_counted_ == occlusion_draws_ &&
                     (!occlusion_counted_ || occlusion_flush_ == stats_.flushes);
  occlusion_open_ = false;
  std::lock_guard lock(occlusion_mutex_);
  const uint64_t generation = ++occlusion_generations_[address];
  if (!exact) {
    ++stats_.occlusion_inexact;
    WriteOcclusionResult(address, generation, occlusion_visible_);
    return;
  }
  // Zero draws (a tile's replay, or an empty query) still waits its turn: see the top of the file.
  s.occlusion.push_back({address, occlusion_counted_ ? occlusion_first_ : 0, occlusion_counted_, generation});}

bool Renderer::occlusion_unsubmitted() const { return !submissions_[current_].occlusion.empty(); }

void Renderer::WriteOcclusionResult(uint32_t address, uint64_t generation, uint32_t samples) {
  // A later query that ended at the same address owns it now.
  if (auto it = occlusion_generations_.find(address); it == occlusion_generations_.end() || it->second != generation) {
    return;
  }
  auto* counts = reinterpret_cast<xe::xe_gpu_depth_sample_counts*>(const_cast<uint8_t*>(memory_(address)));
  if (!counts) return;
  counts->Total_A = samples, counts->Total_B = 0;
  counts->ZFail_A = 0, counts->ZFail_B = 0;
  counts->StencilFail_A = 0, counts->StencilFail_B = 0;
  // Both ZPass words at once (the done check reads them): A = samples, B = 0, little-endian.
  if (reinterpret_cast<uintptr_t>(&counts->ZPass_A) % 8 == 0) {
    std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(&counts->ZPass_A)).store(samples, std::memory_order_release);
  } else {
    counts->ZPass_B = 0;
    std::atomic_thread_fence(std::memory_order_release);
    counts->ZPass_A = samples;
  }
}

void Renderer::WaitResolved(uint32_t index) {
  if (!occlusion_pool_size_) return;
  std::unique_lock lock(occlusion_mutex_);
  occlusion_cv_.wait(lock, [&] { return !submissions_[index].resolving; });
}

void Renderer::QueueResolve(uint32_t index) {
  Submission& s = submissions_[index];
  if (s.occlusion.empty()) return;
  {
    std::lock_guard lock(occlusion_mutex_);
    s.resolving = true;
    occlusion_jobs_.push_back(index);
  }
  occlusion_cv_.notify_all();
}

void Renderer::StopOcclusion() {
  if (!occlusion_thread_.joinable()) return;
  {
    std::lock_guard lock(occlusion_mutex_);
    occlusion_stop_ = true;
  }
  occlusion_cv_.notify_all();
  occlusion_thread_.join();
}

void Renderer::ResolveOcclusion() {
  std::vector<uint64_t> results;
  for (;;) {
    uint32_t index;
    {
      std::unique_lock lock(occlusion_mutex_);
      occlusion_cv_.wait(lock, [&] { return occlusion_stop_ || !occlusion_jobs_.empty(); });
      if (occlusion_jobs_.empty()) return;
      index = occlusion_jobs_.front();
      occlusion_jobs_.pop_front();
    }
    Submission& s = submissions_[index];
    // The CP thread leaves this submission alone until `resolving` is cleared (WaitResolved).
    while (vk_.vkWaitForFences(device_->device(), 1, &s.fence, VK_TRUE, 100'000'000) == VK_TIMEOUT) {
    }
    results.assign(s.occlusion_slots, 0);
    const bool read = !s.occlusion_slots ||
                      vk_.vkGetQueryPoolResults(device_->device(), s.occlusion_pool, 0, s.occlusion_slots,
                                                results.size() * 8, results.data(), 8,
                                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS;
    {
      std::lock_guard lock(occlusion_mutex_);
      for (const OcclusionQuery& q : s.occlusion) {
        uint64_t samples = 0;
        for (uint32_t i = 0; i < q.count; ++i) samples += results[q.first + i];
        if (!read) samples = occlusion_visible_;
        if (!samples && q.count) occlusion_hidden_.fetch_add(1, std::memory_order_relaxed);
        WriteOcclusionResult(q.address, q.generation, uint32_t(std::min<uint64_t>(samples, UINT32_MAX)));
      }
      s.occlusion.clear();
      s.resolving = false;
    }
    occlusion_cv_.notify_all();
  }
}

}  // namespace bots::gfx::vk
