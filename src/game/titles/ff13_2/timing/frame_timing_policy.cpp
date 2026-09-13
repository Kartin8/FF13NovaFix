#include "game/titles/ff13_2/timing/frame_timing_policy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace novafix::game::xiii2_frame_timing_policy {

float Xiii2FrameSourceRate(float requestedRate, std::int32_t gameMode,
                           bool enabled, std::uint32_t configuredLimit) {
    constexpr float kSteamNormalRate = 59.94006f;
    constexpr float kXboxNormalRate = 29.97003f;
    constexpr float kRateTolerance = 0.001f;
    const bool supportedProfile = configuredLimit == 0u || configuredLimit >= 30u;
    if (!enabled || !supportedProfile || gameMode != 1 ||
        !std::isfinite(requestedRate) ||
        std::fabs(requestedRate - kSteamNormalRate) > kRateTolerance) {
        return requestedRate;
    }
    return kXboxNormalRate;
}

bool Xiii2CanDecouplePresentation(std::int32_t gameMode) {
    return gameMode >= 0 && gameMode <= 2;
}

std::uint32_t Xiii2WallDeltaTicks(std::uint64_t elapsedCounter,
                                  std::uint64_t counterFrequency) {
    constexpr double kEngineTicksPerSecond = 300000.0;
    if (!counterFrequency) return 1u;
    const double ticks =
        static_cast<double>(elapsedCounter) * kEngineTicksPerSecond /
        static_cast<double>(counterFrequency);
    const double bounded = std::clamp(
        std::round(ticks), 1.0,
        static_cast<double>((std::numeric_limits<std::uint32_t>::max)()));
    return static_cast<std::uint32_t>(bounded);
}

float Xiii2VeffDeltaMilliseconds(float taskDeltaMilliseconds,
                                 std::uint32_t wallTicks,
                                 bool highRefreshActive) {
    constexpr float kEngineTicksPerMillisecond = 300.0f;
    if (!highRefreshActive || wallTicks == 0u) return taskDeltaMilliseconds;
    return static_cast<float>(wallTicks) / kEngineTicksPerMillisecond;
}

std::uint32_t Xiii2RootMotionTicks(std::uint32_t taskTicks,
                                  std::uint32_t wallTicks,
                                  bool highRefreshActive,
                                  bool ownsAuthoredQuantum) {
    if (!highRefreshActive || !ownsAuthoredQuantum || wallTicks == 0u) {
        return taskTicks;
    }
    return wallTicks;
}

} // namespace novafix::game::xiii2_frame_timing_policy
