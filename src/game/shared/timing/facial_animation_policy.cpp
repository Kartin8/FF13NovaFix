#include "game/shared/timing/facial_animation_policy.h"

#include <algorithm>
#include <cmath>

namespace novafix::game::facial_animation_policy {
namespace {

constexpr float kHighFpsThresholdMs = 32.0f;
constexpr std::int32_t kMinimumTicks = 750;  // 400 FPS
constexpr std::int32_t kMaximumTicks = 9600; // 31.25 FPS

std::int32_t ClampTicks(double value) {
    return std::clamp(static_cast<std::int32_t>(std::lround(value)),
                      kMinimumTicks, kMaximumTicks);
}

} // namespace

std::int32_t ExpectedTicks(float frameMilliseconds, unsigned targetRate) {
    if (std::isfinite(frameMilliseconds) && frameMilliseconds > 0.0f) {
        if (frameMilliseconds >= kHighFpsThresholdMs) return 0;
        return ClampTicks(static_cast<double>(frameMilliseconds) *
                          static_cast<double>(kEngineTicksPerSecond) / 1000.0);
    }
    if (targetRate <= 30) return 0;
    return ClampTicks(static_cast<double>(kEngineTicksPerSecond) /
                      static_cast<double>(targetRate));
}

bool ShouldNormalize(std::int32_t gameTicks, std::int32_t expectedTicks,
                     std::int32_t referenceTicks) {
    return expectedTicks > 0 && gameTicks >= kMinimumTicks &&
           referenceTicks > kMinimumTicks && gameTicks < referenceTicks;
}

std::uint8_t ScaleLinearTransitionByte(std::uint8_t value,
                                       std::int32_t gameTicks,
                                       std::int32_t referenceTicks) {
    if (value == 0 || gameTicks <= 0 || referenceTicks <= 0 ||
        gameTicks >= referenceTicks) {
        return value;
    }
    // FacialController derives this byte from 1 / transitionFrames. It is a
    // linear step consumed once per update, unlike the separate 0.2/0.8
    // smoothing filters below
    const double scaled = static_cast<double>(value) *
                          static_cast<double>(gameTicks) /
                          static_cast<double>(referenceTicks);
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::lround(scaled)), 1, 255));
}

float TimeCorrectedAlpha(float referenceAlpha, std::int32_t gameTicks,
                         std::int32_t referenceTicks) {
    if (!std::isfinite(referenceAlpha) || referenceAlpha <= 0.0f) return 0.0f;
    if (referenceAlpha >= 1.0f) return 1.0f;
    if (gameTicks <= 0 || referenceTicks <= 0 ||
        gameTicks >= referenceTicks) return referenceAlpha;
    const double referenceFrames = static_cast<double>(gameTicks) /
                                   static_cast<double>(referenceTicks);
    return static_cast<float>(
        1.0 - std::pow(1.0 - referenceAlpha, referenceFrames));
}

std::int32_t ResolveFacialTicks(std::int32_t gameTicks,
                               std::uint32_t wallTicks,
                               bool useWallDelta,
                               std::int32_t referenceTicks) {
    if (!useWallDelta || referenceTicks <= kMinimumTicks ||
        wallTicks < static_cast<std::uint32_t>(kMinimumTicks) ||
        wallTicks >= static_cast<std::uint32_t>(referenceTicks)) {
        return gameTicks;
    }
    return static_cast<std::int32_t>(wallTicks);
}

} // namespace novafix::game::facial_animation_policy
