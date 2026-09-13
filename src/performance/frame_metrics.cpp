#include "performance/frame_metrics.h"

#include "settings/performance_settings.h"

#include <d3d9.h>

#include <atomic>
#include <cstdint>

namespace novafix::performance {
namespace {

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
LARGE_INTEGER g_frequency{};
SRWLOCK g_lock = SRWLOCK_INIT;
FrameStatistics g_statistics;
std::int64_t g_lastBegin{};
float g_pendingFrameMs{};
unsigned g_pendingTarget{};
std::atomic_bool g_lastPresentSucceeded{false};
bool g_previousActive{};
std::atomic_uint g_refreshRate{0};
std::atomic<float> g_currentFrameMs{0.0f};
std::atomic_uint g_targetRate{0};
ULONGLONG g_lastRefreshQuery{};

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    QueryPerformanceFrequency(&g_frequency);
    return TRUE;
}

float Milliseconds(std::int64_t ticks) {
    if (g_frequency.QuadPart <= 0) return 0.0f;
    return static_cast<float>(static_cast<double>(ticks) * 1000.0 /
                              static_cast<double>(g_frequency.QuadPart));
}

unsigned EffectiveTarget(const settings::PerformanceSettings& profile, unsigned refreshRate) {
    if (profile.vsyncMode == settings::VSyncMode::On && refreshRate &&
        (profile.frameRateLimit == 0 || profile.frameRateLimit >= refreshRate)) {
        return refreshRate;
    }
    return profile.frameRateLimit ? profile.frameRateLimit : refreshRate;
}

void RefreshDisplayMode(IDirect3DDevice9* device) {
    const ULONGLONG now = GetTickCount64();
    if (!device || now - g_lastRefreshQuery < 1000) return;
    g_lastRefreshQuery = now;
    D3DDISPLAYMODE mode{};
    if (SUCCEEDED(device->GetDisplayMode(0, &mode)) && mode.RefreshRate) {
        g_refreshRate.store(mode.RefreshRate, std::memory_order_relaxed);
    }
}

} // namespace

void BeginFrame(IDirect3DDevice9* device, HWND window) {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
    RefreshDisplayMode(device);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const bool active = window && GetForegroundWindow() == window && !IsIconic(window);
    const unsigned refresh = g_refreshRate.load(std::memory_order_relaxed);
    const unsigned target = EffectiveTarget(settings::ActivePerformance(), refresh);
    const bool lastPresentSucceeded =
        g_lastPresentSucceeded.load(std::memory_order_acquire);

    AcquireSRWLockExclusive(&g_lock);
    if (!active || !g_previousActive || !g_lastBegin) {
        g_pendingFrameMs = 0.0f;
    } else {
        g_pendingFrameMs = Milliseconds(now.QuadPart - g_lastBegin);
        if (g_pendingFrameMs > 1000.0f) g_pendingFrameMs = 0.0f;
        if (lastPresentSucceeded && g_pendingFrameMs > 0.0f) {
            g_statistics.Add(g_pendingFrameMs, g_pendingTarget);
        }
    }
    g_pendingTarget = target;
    g_currentFrameMs.store(g_pendingFrameMs, std::memory_order_relaxed);
    g_targetRate.store(target, std::memory_order_relaxed);
    g_lastBegin = now.QuadPart;
    g_previousActive = active;
    ReleaseSRWLockExclusive(&g_lock);
}

void EndFrame(HRESULT result) {
    g_lastPresentSucceeded.store(SUCCEEDED(result), std::memory_order_release);
}

FrameStatisticsSnapshot CurrentFrameStatistics() {
    AcquireSRWLockShared(&g_lock);
    const FrameStatisticsSnapshot result = g_statistics.Snapshot();
    ReleaseSRWLockShared(&g_lock);
    return result;
}

unsigned ObservedRefreshRate() {
    return g_refreshRate.load(std::memory_order_relaxed);
}

float CurrentFrameMilliseconds() {
    return g_currentFrameMs.load(std::memory_order_relaxed);
}

unsigned CurrentTargetRate() {
    return g_targetRate.load(std::memory_order_relaxed);
}

} // namespace novafix::performance
