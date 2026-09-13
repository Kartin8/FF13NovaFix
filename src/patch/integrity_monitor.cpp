#include "patch/integrity_monitor.h"

#include "diagnostics/log.h"
#include "patch/registry.h"

#include <windows.h>
#include <process.h>

#include <atomic>
#include <cstdint>

namespace novafix::patch::integrity_monitor {
namespace {

constexpr DWORD kVerificationIntervalMs = 250u;
constexpr std::size_t kSiteBudget = 4u;

INIT_ONCE g_startOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_running{false};
std::atomic<ULONGLONG> g_lastFallbackCheck{};

unsigned __stdcall MonitorThread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Sleep(kVerificationIntervalMs);
        registry::VerifyIntegrity(kSiteBudget);
    }
}

BOOL CALLBACK StartMonitor(PINIT_ONCE, PVOID, PVOID*) {
    const std::uintptr_t thread = _beginthreadex(
        nullptr, 0u, &MonitorThread, nullptr, 0u, nullptr);
    if (!thread) {
        LogWarning("Patch integrity monitor failed to start: fallback=presentation");
        return TRUE;
    }

    CloseHandle(reinterpret_cast<HANDLE>(thread));
    g_running.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Start() {
    InitOnceExecuteOnce(&g_startOnce, &StartMonitor, nullptr, nullptr);
}

void PumpFallback() {
    if (g_running.load(std::memory_order_acquire)) return;

    const ULONGLONG now = GetTickCount64();
    ULONGLONG previous =
        g_lastFallbackCheck.load(std::memory_order_relaxed);
    if (now - previous < kVerificationIntervalMs ||
        !g_lastFallbackCheck.compare_exchange_strong(
            previous, now, std::memory_order_relaxed)) {
        return;
    }
    registry::VerifyIntegrity(kSiteBudget);
}

} // namespace novafix::patch::integrity_monitor
