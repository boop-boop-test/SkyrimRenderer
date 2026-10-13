#include "SkyrimRenderer/WorkerPool.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <exception>
#include <utility>
#include <windows.h>

namespace SkyrimRenderer {

namespace {
// Leave two cores to the engine (main + render threads), at most six workers.
constexpr uint32_t kMaxWorkers = 6;
constexpr uint32_t kReservedCores = 2;

// fn(i) with a throw logged and the first one kept for the caller, so every
// item runs whether the job is spread over the pool or run inline.
void RunItem(const std::function<void(uint32_t)>& fn, uint32_t i, std::mutex& errorMutex,
             std::exception_ptr& error) {
    std::exception_ptr thrown;
    try {
        fn(i);
        return;
    } catch (const std::exception& e) {
        SKSE::log::error("[SkyrimRenderer] Worker job {} threw: {}", i, e.what());
        thrown = std::current_exception();
    } catch (...) {
        SKSE::log::error("[SkyrimRenderer] Worker job {} threw", i);
        thrown = std::current_exception();
    }
    std::lock_guard lock(errorMutex);
    if (!error) error = std::move(thrown);
}
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
    limit_.store(n, std::memory_order_relaxed);
    SKSE::log::info("[SkyrimRenderer] Worker pool: {} threads ({} hardware threads)", n, hw);
}

uint32_t WorkerPool::SetLimit(int32_t limit) {
    const uint32_t applied = limit < 0 ? WorkerCount() : std::min(static_cast<uint32_t>(limit), WorkerCount());
    limit_.store(applied, std::memory_order_relaxed);
    SKSE::log::info("[SkyrimRenderer] Worker pool: using {} of {} workers", applied, WorkerCount());
    return applied;
}

void WorkerPool::RunItems() {
    for (;;) {
        const uint32_t i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= count_) return;
        RunItem(*fn_, i, errorMutex_, error_);
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
    const uint32_t limit = WorkerLimit();
    bool idle = false;
    if (count == 1 || limit == 0 || !busy_.compare_exchange_strong(idle, true)) {
        std::mutex errorMutex;
        std::exception_ptr error;
        for (uint32_t i = 0; i < count; ++i) RunItem(fn, i, errorMutex, error);
        if (error) std::rethrow_exception(error);
        return;
    }

    {
        std::lock_guard lock(mutex_);
        fn_ = &fn;
        count_ = count;
        next_.store(0, std::memory_order_relaxed);
        active_ = 0;
        slots_ = std::min(limit, count - 1);  // the caller takes one share
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
    std::exception_ptr error;
    {
        std::lock_guard lock(errorMutex_);
        error = std::exchange(error_, nullptr);
    }
    busy_.store(false);
    if (error) std::rethrow_exception(error);
}

} // namespace SkyrimRenderer
