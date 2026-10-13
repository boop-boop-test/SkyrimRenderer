#include "SkyrimRenderer/WorkerPool.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <windows.h>

namespace SkyrimRenderer {

namespace {
// Leave two cores to the engine (main + render threads), at most six workers.
constexpr uint32_t kMaxWorkers = 6;
constexpr uint32_t kReservedCores = 2;
}

WorkerPool& WorkerPool::Get() {
    // Never destroyed: workers sleep until process exit, so nothing joins a
    // thread from a DLL detach or static destruction.
    static WorkerPool* pool = new WorkerPool();
    return *pool;
}

WorkerPool::WorkerPool() {
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t n = hw > kReservedCores ? std::min(kMaxWorkers, hw - kReservedCores) : 0;
    workers_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        workers_.emplace_back([this] { WorkerMain(); });
        SetThreadDescription(workers_.back().native_handle(), L"SkyrimRenderer worker");
    }
    SKSE::log::info("[SkyrimRenderer] Worker pool: {} threads ({} hardware threads)", n, hw);
}

void WorkerPool::RunItems() {
    for (;;) {
        const uint32_t i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= count_) return;
        try {
            (*fn_)(i);
        } catch (const std::exception& e) {
            SKSE::log::error("[SkyrimRenderer] Worker job {} threw: {}", i, e.what());
        } catch (...) {
            SKSE::log::error("[SkyrimRenderer] Worker job {} threw", i);
        }
    }
}

void WorkerPool::WorkerMain() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return slots_ > 0; });
        --slots_;
        ++active_;
        lock.unlock();
        RunItems();
        lock.lock();
        if (--active_ == 0) done_.notify_all();
    }
}

void WorkerPool::ParallelFor(uint32_t count, const std::function<void(uint32_t)>& fn) {
    if (count == 0) return;
    bool idle = false;
    if (count == 1 || workers_.empty() || !busy_.compare_exchange_strong(idle, true)) {
        for (uint32_t i = 0; i < count; ++i) fn(i);
        return;
    }

    {
        std::lock_guard lock(mutex_);
        fn_ = &fn;
        count_ = count;
        next_.store(0, std::memory_order_relaxed);
        active_ = 0;
        slots_ = std::min(WorkerCount(), count - 1);  // the caller takes one share
    }
    wake_.notify_all();

    RunItems();

    {
        // Workers that have not woken yet are not needed any more: cancel their
        // slots and wait only for the ones already inside the job.
        std::unique_lock lock(mutex_);
        slots_ = 0;
        done_.wait(lock, [this] { return active_ == 0; });
        fn_ = nullptr;
        count_ = 0;
    }
    busy_.store(false);
}

} // namespace SkyrimRenderer
