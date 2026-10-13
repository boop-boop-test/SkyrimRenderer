#include "SkyrimRenderer/WorkerPool.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <exception>
#include <utility>
#include <windows.h>

namespace SkyrimRenderer {

namespace {
// Leave two cores to the engine (main + render threads), at most six workers.
constexpr uint32_t kMaxWorkers = 6;
constexpr uint32_t kReservedCores = 2;
constexpr const char* kLimitPath = "Data/SKSE/Plugins/SkyrimRenderer_Workers.txt";
constexpr auto kLimitCheckInterval = std::chrono::seconds(2);

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

void WorkerPool::RefreshLimit() {
    std::unique_lock lock(limitMutex_, std::try_to_lock);
    if (!lock) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < nextLimitCheck_) return;
    nextLimitCheck_ = now + kLimitCheckInterval;

    std::error_code ec;
    const auto time = std::filesystem::last_write_time(kLimitPath, ec);
    const bool present = !ec;
    if (present == limitFilePresent_ && (!present || time == limitFileTime_)) return;
    limitFilePresent_ = present;
    limitFileTime_ = present ? time : std::filesystem::file_time_type{};

    uint32_t limit = WorkerCount();
    if (present) {
        std::ifstream in(kLimitPath);
        for (std::string line; std::getline(in, line);) {
            const auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            unsigned value = 0;
            if (std::sscanf(line.c_str(), " workers = %u", &value) == 1) {
                limit = std::min<uint32_t>(value, WorkerCount());
            }
        }
    }
    limit_.store(limit, std::memory_order_relaxed);
    SKSE::log::info("[SkyrimRenderer] Worker pool: using {} of {} workers{}", limit, WorkerCount(),
                    present ? " (SkyrimRenderer_Workers.txt)" : "");
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
    RefreshLimit();
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
