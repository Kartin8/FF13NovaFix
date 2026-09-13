#pragma once

#include <string_view>

namespace novafix::settings {

enum class VSyncMode : unsigned {
    GameDefault,
    Off,
    On,
};

enum class TripleBufferingMode : unsigned {
    Auto,
    Off,
    On,
};

enum class D3D9Backend : unsigned {
    Automatic,
    System,
    LocalDXVK,
};

enum class LoadedModulesNotificationMode : unsigned {
    Never,
    WhenChanged,
    EveryLaunch,
};

struct PerformanceSettings {
    unsigned frameRateLimit{0}; // 0 = unlimited
    bool smartControllerHotplug{true};
    VSyncMode vsyncMode{VSyncMode::Off};
    unsigned refreshRate{0}; // 0 = system default
    TripleBufferingMode tripleBuffering{TripleBufferingMode::Auto};
    bool schedulingStability{true};
    bool highFpsCorrectness{true};
    bool xiii2OptimizeStateSubmission{true};
    bool showFpsCounter{false};
    LoadedModulesNotificationMode loadedModulesNotificationMode{
        LoadedModulesNotificationMode::WhenChanged};
    D3D9Backend d3d9Backend{D3D9Backend::Automatic};
};

void SetPerformancePresentationControlsEnabled(bool enabled);
PerformanceSettings ActivePerformance();
PerformanceSettings StoredPerformance();
void PreviewPerformance(const PerformanceSettings& value);
bool SavePerformance(const PerformanceSettings& value);
bool IsLoadedModulesNotificationSignatureCurrent(
    std::wstring_view signature);
bool SaveLoadedModulesNotificationSignature(std::wstring_view signature);

} // namespace novafix::settings
