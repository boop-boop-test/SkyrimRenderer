#include "SkyrimRenderer/WorkerPool.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>
#include <windows.h>

namespace SkyrimRenderer {

namespace {
// Leave two cores to the engine (main + render threads), at most six workers.
constexpr uint32_t kMaxWorkers = 6;
constexpr uint32_t kReservedCores = 2;
// Workers used by default: half the physical cores minus one, at most three.
// Measured in game (NTJH, 8-core / 16-thread Ryzen 3700X, two driven heads):
// prep wall time flattens at 2-3 workers while every added worker inflates
// total prep CPU (1.7 ms at 0 -> 3.5 at 3 -> 4.8 at 6, per head), because the
// game's own threads are busy at the same time. 3 workers = 6's wall time for
// ~25% less CPU. The pool keeps kMaxWorkers threads for SetLimit to raise to.
constexpr uint32_t kMaxDefaultWorkers = 3;

uint32_t PhysicalCoreCount() {
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes == 0) return 0;
    std::vector<uint8_t> buffer(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) return 0;
    uint32_t cores = 0;
    for (DWORD offset = 0; offset < bytes;) {
        auto* entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) ++cores;
        offset += entry->Size;
    }
    return cores;
}

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
    uint32_t cores = PhysicalCoreCount();
    if (cores == 0) cores = std::max(1u, hw / 2);  // query failed: assume SMT
    defaultLimit_ = std::min({n, kMaxDefaultWorkers, cores > 2 ? cores / 2 - 1 : 0u});
    limit_.store(defaultLimit_, std::memory_order_relaxed);
    SKSE::log::info("[SkyrimRenderer] Worker pool: {} threads, using {} by default ({} cores, {} hardware threads)",
                    n, defaultLimit_, cores, hw);
}

uint32_t WorkerPool::SetLimit(int32_t limit) {
    const uint32_t applied = limit < 0 ? defaultLimit_ : std::min(static_cast<uint32_t>(limit), WorkerCount());
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
