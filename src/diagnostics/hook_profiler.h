#pragma once

#include <windows.h>

#include <cstdint>
#include <string_view>

namespace novafix::hook_profiler {

enum class Point : std::uint8_t {
    FeatureUpdates,
    ControllerVibrationBackend,
    ControllerVibrationNonZero,
    ControllerVibrationSetState,
    DirectInputGetStateOwn,
    DirectInputGetStateOriginal,
    DirectInputControllerGetStateOriginal,
    DirectInputKeyboardGetStateOriginal,
    DirectInputMouseGetStateOriginal,
    DirectInputUnknownGetStateOriginal,
    DirectInputControllerChangedGetStateOriginal,
    DirectInputControllerUnchangedGetStateOriginal,
    DirectInputControllerLatencyUnderPointOneMs,
    DirectInputControllerLatencyPointOneToOneMs,
    DirectInputControllerLatencyOneToFourMs,
    DirectInputControllerLatencyFourToEightMs,
    DirectInputControllerLatencyEightToSixteenMs,
    DirectInputControllerLatencySixteenMsOrMore,
    DirectInputControllerTransientFailure,
    DirectInputControllerOtherFailure,
    DirectInputObserveState,
    DirectInputGetDataOwn,
    DirectInputGetDataOriginal,
    LrPromptRefreshOwn,
    LrPromptRefreshOriginal,
    LrPromptGuideRebuild,
    LrPromptLabelLookup,
    LrPromptCapturedSourceCopy,
    LrPromptLabelOriginal,
    LrPromptLabelBookkeeping,
    LrPromptLabelDestroyOwn,
    LrPromptLabelDestroyOriginal,
    LrXInputOriginal,
    LrXInputOwn,
    LrPlatformQueryOriginal,
    LrPlatformQueryOwn,
    LrSchemataNavigateOwn,
    LrSchemataNavigateOriginal,
    LrSchemataConfirmOwn,
    LrSchemataConfirmOriginal,
    LrSchemataUpdateOwn,
    LrSchemataUpdateOriginal,
    Count,
};

struct DynamicPoint {
    static constexpr std::uint16_t kInvalid = 0xFFFFu;
    std::uint16_t index{kInvalid};
};

DynamicPoint RegisterDynamicPoint(std::string_view name);

// Records caller-measured ticks so one timer can feed multiple counters
// Call only while Debug logging is enabled
void RecordMeasuredTicks(Point point, std::uint64_t ticks);

class Scope {
public:
    explicit Scope(Point point);
    Scope(Point point, bool selected, std::uint32_t weight);
    explicit Scope(DynamicPoint point);
    Scope(DynamicPoint point, bool selected, std::uint32_t weight);
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope();

    void Stop();

private:
    std::uint16_t index_{DynamicPoint::kInvalid};
    std::int64_t start_{};
    std::uint32_t weight_{1u};
    bool active_{};
};

// Reports aggregate timings every 10s while Debug logging is enabled
// Call from one frame-boundary thread, never from a measured hook
void ReportIfDue();

} // namespace novafix::hook_profiler

// Function-local named scope; registers its label once and records only in Debug
// Variable names must be unique within the function
#define NOVAFIX_PROFILE_NAMED_SCOPE(variable, label)                         \
    static const auto variable##_point =                                     \
        ::novafix::hook_profiler::RegisterDynamicPoint(label);               \
    ::novafix::hook_profiler::Scope variable(variable##_point)

// Samples high-frequency call sites at a power-of-two interval. Calls, total
// time, and window percentage are extrapolated; average and max are measured samples
#define NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(variable, label, interval)       \
    static const auto variable##_point =                                     \
        ::novafix::hook_profiler::RegisterDynamicPoint(label);               \
    thread_local std::uint32_t variable##_sample_counter{};                  \
    const bool variable##_selected =                                         \
        (variable##_sample_counter++ & ((interval) - 1u)) == 0u;             \
    ::novafix::hook_profiler::Scope variable(                                \
        variable##_point, variable##_selected, (interval))
