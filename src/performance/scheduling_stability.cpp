#include "performance/scheduling_stability.h"

#include "compat/platform.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <atomic>

namespace novafix::performance {
namespace {

struct PowerThrottlingState {
    ULONG version;
    ULONG controlMask;
    ULONG stateMask;
};

constexpr ULONG kPowerThrottlingCurrentVersion = 1;
constexpr ULONG kPowerThrottlingExecutionSpeed = 1;
constexpr int kProcessPowerThrottlingClass = 4;
constexpr int kThreadPowerThrottlingClass = 3;

using SetProcessInformationFn = BOOL (WINAPI*)(HANDLE, int, void*, DWORD);
using SetThreadInformationFn = BOOL (WINAPI*)(HANDLE, int, void*, DWORD);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
bool g_enabled = false;
std::atomic_bool g_supported{false};
SetThreadInformationFn g_setThreadInformation = nullptr;
ULONGLONG g_lastProcessRefresh{};

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    g_enabled = settings::ActivePerformance().schedulingStability;
    if (!g_enabled) {
        return TRUE;
    }
    if (compat::CurrentEnvironment().host != compat::Host::Windows) {
        return TRUE;
    }
    g_supported.store(true, std::memory_order_relaxed);

    const HANDLE process = GetCurrentProcess();
    const DWORD priority = GetPriorityClass(process);
    if (priority == IDLE_PRIORITY_CLASS ||
        priority == BELOW_NORMAL_PRIORITY_CLASS ||
        priority == NORMAL_PRIORITY_CLASS) {
        SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS);
    }

    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto setProcessInformation = kernel ? reinterpret_cast<SetProcessInformationFn>(
        GetProcAddress(kernel, "SetProcessInformation")) : nullptr;
    g_setThreadInformation = kernel ? reinterpret_cast<SetThreadInformationFn>(
        GetProcAddress(kernel, "SetThreadInformation")) : nullptr;
    PowerThrottlingState state{
        kPowerThrottlingCurrentVersion, kPowerThrottlingExecutionSpeed, 0};
    if (setProcessInformation) {
        setProcessInformation(
            process, kProcessPowerThrottlingClass, &state, sizeof(state));
    }
    return TRUE;
}

} // namespace

void PumpSchedulingStability() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
    if (!g_enabled || !g_supported.load(std::memory_order_relaxed)) return;

    const ULONGLONG now = GetTickCount64();
    if (!g_lastProcessRefresh || now - g_lastProcessRefresh >= 2000) {
        g_lastProcessRefresh = now;
        const DWORD processPriority = GetPriorityClass(GetCurrentProcess());
        if ((processPriority == IDLE_PRIORITY_CLASS ||
             processPriority == BELOW_NORMAL_PRIORITY_CLASS ||
             processPriority == NORMAL_PRIORITY_CLASS)) {
            SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        }
    }

    const HANDLE thread = GetCurrentThread();
    thread_local ULONGLONG lastThreadRefresh = 0;
    if (lastThreadRefresh && now - lastThreadRefresh < 2000) return;
    lastThreadRefresh = now;
    const int priority = GetThreadPriority(thread);
    if (priority != THREAD_PRIORITY_ERROR_RETURN &&
        priority < THREAD_PRIORITY_ABOVE_NORMAL) {
        SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    }
    PowerThrottlingState state{
        kPowerThrottlingCurrentVersion, kPowerThrottlingExecutionSpeed, 0};
    if (g_setThreadInformation) {
        g_setThreadInformation(
            thread, kThreadPowerThrottlingClass, &state, sizeof(state));
    }
}

} // namespace novafix::performance
