#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace SkyrimRenderer {

/// Small persistent fork-join pool for per-frame CPU work (driven-head vertex
/// prep). The calling thread takes part, so a ParallelFor of one item never
/// leaves it. Jobs must not touch game objects or the game's allocators: the
/// workers are plain OS threads the engine knows nothing about.
///
/// One ParallelFor at a time (the render path runs on the game's main thread);
/// a nested or concurrent call runs inline on its caller.
///
/// SetLimit caps the workers a ParallelFor may use (SkyrimRenderer_SetPrepWorkers).
class WorkerPool {
public:
    static WorkerPool& Get();

    /// Run fn(i) for i in [0, count) across the pool and the caller; returns
    /// when every call has finished. A call that throws is logged and the rest
    /// still run; the first exception is then rethrown on the caller (inline
    /// or pooled alike).
    void ParallelFor(uint32_t count, const std::function<void(uint32_t)>& fn);

    uint32_t WorkerCount() const { return static_cast<uint32_t>(workers_.size()); }
    /// Workers a ParallelFor may use (WorkerCount unless capped).
    uint32_t WorkerLimit() const { return limit_.load(std::memory_order_relaxed); }
    /// Cap the workers per ParallelFor: 0 = all on the calling thread, at most
    /// WorkerCount; negative = back to WorkerCount. Returns the applied cap.
    uint32_t SetLimit(int32_t limit);

private:
    WorkerPool();
    void WorkerMain();
    void RunItems();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::atomic<bool> busy_{false};

    // Current job (valid while busy_)
    const std::function<void(uint32_t)>* fn_ = nullptr;
    uint32_t count_ = 0;
    std::atomic<uint32_t> next_{0};
    uint32_t slots_ = 0;       // workers still wanted for the current job
    uint32_t active_ = 0;      // workers inside the current job
    std::mutex errorMutex_;
    std::exception_ptr error_;  // first throw of the current job

    std::atomic<uint32_t> limit_{0};
};

} // namespace SkyrimRenderer
