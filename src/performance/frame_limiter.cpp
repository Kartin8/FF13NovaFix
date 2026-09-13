#include "performance/frame_limiter.h"

#include "diagnostics/log.h"
#include "display/presentation_policy.h"
#include "game/shared/timing/frame_pacing.h"
#include "performance/frame_schedule.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace novafix::performance {
namespace {

constexpr DWORD kCreateWaitableTimerHighResolution = 0x00000002u;
constexpr double kSpinWindowSeconds = 0.0005;

INIT_ONCE g_timerOnce = INIT_ONCE_STATIC_INIT;
HANDLE g_timer = nullptr;
LARGE_INTEGER g_frequency{};
FrameSchedule g_schedule;
SRWLOCK g_pacingLock = SRWLOCK_INIT;
std::atomic_uint64_t g_loggedPolicy{~std::uint64_t{0}};
std::atomic_bool g_scheduleArmed{false};
FramePacingSite g_scheduleSite = FramePacingSite::Presentation;

BOOL CALLBACK InitializeTimer(PINIT_ONCE, PVOID, PVOID*) {
    QueryPerformanceFrequency(&g_frequency);
    g_schedule.SetFrequency(g_frequency.QuadPart);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr,
                                     kCreateWaitableTimerHighResolution,
                                     TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    return TRUE;
}

void WaitUntil(std::int64_t deadline) {
    for (;;) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        const std::int64_t remaining = deadline - counter.QuadPart;
        if (remaining <= 0) return;

        const double seconds = static_cast<double>(remaining) /
                               static_cast<double>(g_frequency.QuadPart);
        if (seconds > kSpinWindowSeconds) {
            const double timerSeconds = seconds - kSpinWindowSeconds;
            if (g_timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -std::max<LONGLONG>(
                    1, static_cast<LONGLONG>(timerSeconds * 10000000.0));
                if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(g_timer, INFINITE);
                    continue;
                }
            }

            const DWORD milliseconds = static_cast<DWORD>(timerSeconds * 1000.0);
            if (milliseconds > 0) {
                Sleep(milliseconds);
                continue;
            }
        }

        if (seconds > 0.00015) {
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
}

} // namespace

void PaceFrame(HWND gameWindow, unsigned observedRefreshRate,
               FramePacingSite site) {
    const bool gameBoundaryOwned =
        game::frame_pacing::UsesGameBoundaryPacing();
    if ((site == FramePacingSite::Presentation && gameBoundaryOwned) ||
        (site == FramePacingSite::Xiii2GameBoundary &&
         !gameBoundaryOwned)) {
        return;
    }

    const settings::PerformanceSettings settings = settings::ActivePerformance();
    const bool shouldWait = display::ExternalLimiterShouldWait(settings, observedRefreshRate);
    const std::uint64_t policyKey =
        (shouldWait ? (std::uint64_t{1} << 63u) : 0u) |
        (static_cast<std::uint64_t>(site) << 48u) |
        ((static_cast<std::uint64_t>(observedRefreshRate) & 0xFFFFu) << 32u) |
        static_cast<std::uint64_t>(settings.frameRateLimit);
    const std::uint64_t previous =
        g_loggedPolicy.load(std::memory_order_relaxed);
    if (previous != policyKey) {
        g_loggedPolicy.store(policyKey, std::memory_order_relaxed);
        Log("Frame limiter policy: site=%s wait=%d limit=%u refresh=%u vsync=%u",
            site == FramePacingSite::Xiii2GameBoundary
                ? "xiii2-game-boundary" : "presentation",
            shouldWait, settings.frameRateLimit, observedRefreshRate,
            static_cast<unsigned>(settings.vsyncMode));
    }
    const bool activeWait = shouldWait &&
        game::frame_pacing::IsActive() && gameWindow && !IsIconic(gameWindow);
    if (!activeWait) {
        if (!g_scheduleArmed.load(std::memory_order_acquire)) return;
        AcquireSRWLockExclusive(&g_pacingLock);
        if (g_scheduleArmed.exchange(false, std::memory_order_acq_rel)) {
            g_schedule.Reset();
        }
        ReleaseSRWLockExclusive(&g_pacingLock);
        return;
    }

    InitOnceExecuteOnce(&g_timerOnce, &InitializeTimer, nullptr, nullptr);
    AcquireSRWLockExclusive(&g_pacingLock);
    if (!g_scheduleArmed.load(std::memory_order_relaxed) ||
        g_scheduleSite != site) {
        g_schedule.Reset();
        g_scheduleSite = site;
    }
    g_scheduleArmed.store(true, std::memory_order_release);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const auto deadline = site == FramePacingSite::Xiii2GameBoundary
        ? (settings.frameRateLimit == 60u
            ? g_schedule.NextAlignedDeadline(
                now.QuadPart, 60000u, 1001u)
            : g_schedule.NextAlignedDeadline(
                now.QuadPart, settings.frameRateLimit))
        : g_schedule.NextDeadline(
            now.QuadPart, settings.frameRateLimit);
    if (deadline) WaitUntil(*deadline);
    ReleaseSRWLockExclusive(&g_pacingLock);
}

} // namespace novafix::performance
