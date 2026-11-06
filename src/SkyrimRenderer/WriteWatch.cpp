// Diagnostic: a hardware write watchpoint (debug register DR0) on one 8-byte
// field, armed on every thread of the process. A write from any code
// (renderer, game, another plugin) traps once; the handler records the
// writer's call stack, and WriteWatchReport logs it later from a safe place.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimRenderer/WriteWatch.h"

#include <TlHelp32.h>

#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <thread>

namespace SkyrimRenderer {

namespace {

constexpr int kFrames = 28;
constexpr int kHits = 4;

struct Hit {
    void* frames[kFrames];
    unsigned short count;
    DWORD thread;
    uint64_t valueAfter;
};

std::atomic<uintptr_t> g_address{0};
std::atomic<int> g_hits{0};
Hit g_hit[kHits];
std::atomic<int> g_reported{0};
PVOID g_handler = nullptr;

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    auto* ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;
    ctx->Dr6 = 0;
    const uintptr_t address = g_address.load();
    const int n = g_hits.fetch_add(1);
    if (n < kHits && address) {
        auto& hit = g_hit[n];
        hit.count = RtlCaptureStackBackTrace(0, kFrames, hit.frames, nullptr);
        hit.thread = GetCurrentThreadId();
        hit.valueAfter = *reinterpret_cast<const uint64_t*>(address);
    }
    if (n + 1 >= kHits) {
        // Enough: stop trapping on this thread.
        ctx->Dr7 &= ~0x000F0003ull;
        ctx->Dr0 = 0;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void SetOnAllThreads(uintptr_t address) {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = address;
                c.Dr7 &= ~0x000F0003ull;
                if (address) c.Dr7 |= 0x1ull | (0x1ull << 16) | (0x2ull << 18);  // L0, write, 8 bytes
                c.Dr6 = 0;
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}

}  // namespace

void WriteWatchArm(const void* field) {
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, OnException);
    const auto address = reinterpret_cast<uintptr_t>(field);
    if (address & 7) {
        SKSE::log::warn("[SkyrimRenderer] TRACE write watch: {} is not 8-byte aligned", field);
        return;
    }
    g_hits = 0;
    g_reported = 0;
    g_address = address;
    // A helper sets every other thread; it then also sets this one.
    std::thread([address] { SetOnAllThreads(address); }).join();
    SKSE::log::info("[SkyrimRenderer] TRACE write watch armed on {} (value 0x{:X})", field,
                    *reinterpret_cast<const uint64_t*>(address));
}

void WriteWatchDisarm() {
    if (!g_address) return;
    g_address = 0;
    std::thread([] { SetOnAllThreads(0); }).join();
}

void WriteWatchReport() {
    const int hits = std::min(g_hits.load(), kHits);
    for (int n = g_reported.load(); n < hits; ++n) {
        const auto& hit = g_hit[n];
        std::string trace;
        for (unsigned short i = 0; i < hit.count; ++i) {
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(hit.frames[i]), &module) && module) {
                char path[MAX_PATH]{};
                GetModuleFileNameA(module, path, MAX_PATH);
                const char* base = std::strrchr(path, '\\');
                strncpy_s(name, base ? base + 1 : path, _TRUNCATE);
            }
            trace += std::format(" {}+0x{:X}", name,
                                 reinterpret_cast<uintptr_t>(hit.frames[i]) - reinterpret_cast<uintptr_t>(module));
        }
        SKSE::log::warn("[SkyrimRenderer] TRACE write #{} to the watched tint view on thread {} (now 0x{:X}):{}", n + 1,
                        hit.thread, hit.valueAfter, trace);
    }
    g_reported = hits;
}

}  // namespace SkyrimRenderer
