#include "settings/performance_settings.h"

#include "diagnostics/log.h"
#include "settings/config_path.h"
#include "settings/ini_value.h"
#include "settings/named_values.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace novafix::settings {
namespace {

INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK g_lock = SRWLOCK_INIT;
PerformanceSettings g_active{};
PerformanceSettings g_stored{};
std::wstring g_configPath;
std::atomic_uint64_t g_activeGeneration{1};
std::atomic_bool g_presentationControlsEnabled{true};

void ApplyLiveFields(PerformanceSettings& target,
                     const PerformanceSettings& source) {
    target.frameRateLimit = source.frameRateLimit;
    target.smartControllerHotplug = source.smartControllerHotplug;
    target.highFpsCorrectness = source.highFpsCorrectness;
    target.showFpsCounter = source.showFpsCounter;
    target.loadedModulesNotificationMode =
        source.loadedModulesNotificationMode;
}

void Normalize(PerformanceSettings& value) {
    if (value.frameRateLimit != 0) {
        value.frameRateLimit = std::clamp(value.frameRateLimit, 15u, 1000u);
    }
    value.vsyncMode = static_cast<VSyncMode>(
        std::clamp(static_cast<unsigned>(value.vsyncMode), 0u, 2u));
    if (value.refreshRate != 0) value.refreshRate = std::clamp(value.refreshRate, 24u, 1000u);
    value.tripleBuffering = static_cast<TripleBufferingMode>(
        std::clamp(static_cast<unsigned>(value.tripleBuffering), 0u, 2u));
    value.d3d9Backend = static_cast<D3D9Backend>(
        std::clamp(static_cast<unsigned>(value.d3d9Backend), 0u, 2u));
    value.loadedModulesNotificationMode =
        static_cast<LoadedModulesNotificationMode>(std::clamp(
            static_cast<unsigned>(value.loadedModulesNotificationMode),
            0u, 2u));
}

BOOL CALLBACK Load(PINIT_ONCE, PVOID, PVOID*) {
    g_configPath = storage::ConfigFile();
    const bool presentationControls =
        g_presentationControlsEnabled.load(std::memory_order_acquire);

    PerformanceSettings loaded{};
    if (!g_configPath.empty()) {
        if (presentationControls) {
            loaded.frameRateLimit = ini::ReadChoice(
                g_configPath, L"Performance", L"FrameRateLimit",
                names::kFrameRateLimit, loaded.frameRateLimit);
            loaded.vsyncMode = static_cast<VSyncMode>(
                ini::ReadChoice(
                    g_configPath, L"Performance", L"VSyncMode",
                    names::kVSyncMode,
                    static_cast<unsigned>(loaded.vsyncMode)));
            loaded.refreshRate = ini::ReadChoice(
                g_configPath, L"Performance", L"RefreshRate",
                names::kRefreshRate, loaded.refreshRate);
            loaded.tripleBuffering = static_cast<TripleBufferingMode>(
                ini::ReadChoice(
                    g_configPath, L"Performance", L"TripleBuffering",
                    names::kTripleBuffering,
                    static_cast<unsigned>(loaded.tripleBuffering)));
        } else {
            loaded.vsyncMode = VSyncMode::GameDefault;
        }
        loaded.showFpsCounter = ini::ReadBoolean(
            g_configPath, L"Performance", L"ShowFpsCounter",
            loaded.showFpsCounter);
        loaded.smartControllerHotplug = ini::ReadBoolean(
            g_configPath, L"Performance", L"SmartControllerHotplug", loaded.smartControllerHotplug);
        loaded.schedulingStability = ini::ReadBoolean(
            g_configPath, L"Performance", L"SchedulingStability", loaded.schedulingStability);
        loaded.highFpsCorrectness = ini::ReadBoolean(
            g_configPath, L"Performance", L"HighFpsCorrectness", loaded.highFpsCorrectness);
        loaded.xiii2OptimizeStateSubmission = ini::ReadBoolean(
            g_configPath, L"Performance", L"XIII2OptimizeStateSubmission",
            loaded.xiii2OptimizeStateSubmission);
        loaded.loadedModulesNotificationMode =
            static_cast<LoadedModulesNotificationMode>(
                ini::ReadChoice(
                    g_configPath, L"Performance",
                    L"LoadedModulesNotificationMode",
                    names::kLoadedModulesNotificationMode,
                    static_cast<unsigned>(
                        loaded.loadedModulesNotificationMode)));
        loaded.d3d9Backend = static_cast<D3D9Backend>(
            ini::ReadChoice(
                g_configPath, L"Performance", L"D3D9Backend",
                names::kD3D9Backend,
                static_cast<unsigned>(loaded.d3d9Backend)));
    }
    Normalize(loaded);
    g_active = loaded;
    g_stored = loaded;
    Log("Performance settings: presentation-controls=%d limit=%u smart-hotplug=%d vsync=%u refresh=%u triple=%u scheduling=%d high-fps-correctness=%d xiii2-state-submission=%d fps-counter=%d module-notification=%u d3d9-backend=%u",
        presentationControls, loaded.frameRateLimit, loaded.smartControllerHotplug,
        static_cast<unsigned>(loaded.vsyncMode), loaded.refreshRate,
        static_cast<unsigned>(loaded.tripleBuffering), loaded.schedulingStability,
        loaded.highFpsCorrectness, loaded.xiii2OptimizeStateSubmission,
        loaded.showFpsCounter,
        static_cast<unsigned>(loaded.loadedModulesNotificationMode),
        static_cast<unsigned>(loaded.d3d9Backend));
    return TRUE;
}

void EnsureLoaded() {
    InitOnceExecuteOnce(&g_loadOnce, &Load, nullptr, nullptr);
}

void AddSetting(std::vector<ini::Entry>& entries, const wchar_t* key,
                bool value) {
    entries.push_back({key, ini::FormatBoolean(value)});
}

void AddSetting(std::vector<ini::Entry>& entries, const wchar_t* key,
                unsigned value, std::span<const wchar_t* const> names) {
    entries.push_back({key, ini::FormatChoice(value, names)});
}

} // namespace

void SetPerformancePresentationControlsEnabled(bool enabled) {
    g_presentationControlsEnabled.store(enabled, std::memory_order_release);
}

PerformanceSettings ActivePerformance() {
    EnsureLoaded();
    thread_local std::uint64_t cachedGeneration{};
    thread_local PerformanceSettings cached{};
    const std::uint64_t generation =
        g_activeGeneration.load(std::memory_order_acquire);
    if (cachedGeneration == generation) return cached;

    AcquireSRWLockShared(&g_lock);
    cached = g_active;
    cachedGeneration =
        g_activeGeneration.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&g_lock);
    return cached;
}

PerformanceSettings StoredPerformance() {
    EnsureLoaded();
    AcquireSRWLockShared(&g_lock);
    const PerformanceSettings value = g_stored;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

void PreviewPerformance(const PerformanceSettings& requested) {
    EnsureLoaded();
    PerformanceSettings value = requested;
    Normalize(value);
    AcquireSRWLockExclusive(&g_lock);
    ApplyLiveFields(g_active, value);
    g_activeGeneration.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
}

bool SavePerformance(const PerformanceSettings& requested) {
    EnsureLoaded();
    PerformanceSettings value = requested;
    Normalize(value);
    if (g_configPath.empty()) return false;

    const bool presentationControls =
        g_presentationControlsEnabled.load(std::memory_order_acquire);
    std::vector<ini::Entry> entries;
    entries.reserve(10);
    if (presentationControls) {
        AddSetting(entries, L"FrameRateLimit", value.frameRateLimit,
                   names::kFrameRateLimit);
        AddSetting(entries, L"VSyncMode",
                   static_cast<unsigned>(value.vsyncMode), names::kVSyncMode);
        AddSetting(entries, L"RefreshRate", value.refreshRate,
                   names::kRefreshRate);
        AddSetting(entries, L"TripleBuffering",
                   static_cast<unsigned>(value.tripleBuffering),
                   names::kTripleBuffering);
    }
    AddSetting(entries, L"ShowFpsCounter", value.showFpsCounter);
    AddSetting(entries, L"SmartControllerHotplug", value.smartControllerHotplug);
    AddSetting(entries, L"SchedulingStability", value.schedulingStability);
    AddSetting(entries, L"HighFpsCorrectness", value.highFpsCorrectness);
    AddSetting(entries, L"XIII2OptimizeStateSubmission",
               value.xiii2OptimizeStateSubmission);
    AddSetting(entries, L"LoadedModulesNotificationMode",
               static_cast<unsigned>(value.loadedModulesNotificationMode),
               names::kLoadedModulesNotificationMode);
    AddSetting(entries, L"D3D9Backend",
               static_cast<unsigned>(value.d3d9Backend), names::kD3D9Backend);
    const bool saved =
        ini::WriteValues(g_configPath, L"Performance", entries);
    if (saved) {
        AcquireSRWLockExclusive(&g_lock);
        g_stored = value;
        ApplyLiveFields(g_active, value);
        g_activeGeneration.fetch_add(1, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_lock);
    }
    Log("Performance settings saved: config=%d presentation-controls=%d limit=%u smart-hotplug=%d vsync=%u refresh=%u triple=%u scheduling=%d high-fps-correctness=%d xiii2-state-submission=%d fps-counter=%d module-notification=%u d3d9-backend=%u",
        saved, presentationControls, value.frameRateLimit,
        value.smartControllerHotplug,
        static_cast<unsigned>(value.vsyncMode), value.refreshRate,
        static_cast<unsigned>(value.tripleBuffering), value.schedulingStability,
        value.highFpsCorrectness, value.xiii2OptimizeStateSubmission,
        value.showFpsCounter,
        static_cast<unsigned>(value.loadedModulesNotificationMode),
        static_cast<unsigned>(value.d3d9Backend));
    return saved;
}

bool IsLoadedModulesNotificationSignatureCurrent(
    std::wstring_view signature) {
    EnsureLoaded();
    if (g_configPath.empty() || signature.empty()) return false;
    wchar_t stored[64]{};
    const DWORD length = GetPrivateProfileStringW(
        L"Notifications", L"LoadedModulesSignature", L"", stored,
        static_cast<DWORD>(std::size(stored)), g_configPath.c_str());
    return length == signature.size() &&
           std::wstring_view(stored, length) == signature;
}

bool SaveLoadedModulesNotificationSignature(std::wstring_view signature) {
    EnsureLoaded();
    if (g_configPath.empty() || signature.empty()) return false;
    const std::array<ini::Entry, 1> entries{{
        {L"LoadedModulesSignature", std::wstring(signature)},
    }};
    return ini::WriteValues(g_configPath, L"Notifications", entries);
}

} // namespace novafix::settings
