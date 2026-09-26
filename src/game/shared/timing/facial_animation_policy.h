#pragma once

#include <cstdint>

namespace novafix::game::facial_animation_policy {

inline constexpr std::int32_t kEngineTicksPerSecond = 300000;
inline constexpr std::int32_t kReference30HzTicks =
    kEngineTicksPerSecond / 30;
inline constexpr std::int32_t kXiii2Reference30HzTicks = 10010;
inline constexpr std::int32_t kFacialReference60HzTicks =
    kEngineTicksPerSecond / 60;

// Convert measured frame time to the tick unit used by XIII facial animation.
// Return 0 when no correction is needed at the requested cadence
std::int32_t ExpectedTicks(float frameMilliseconds, unsigned targetRate);

// The games advance facial morph transitions with per-update constants. The
// caller supplies the title-specific authored cadence: 30 Hz for XIII and
// XIII-2, and 60 Hz for LR
bool ShouldNormalize(std::int32_t gameTicks, std::int32_t expectedTicks,
                     std::int32_t referenceTicks = kFacialReference60HzTicks);
std::uint8_t ScaleLinearTransitionByte(
    std::uint8_t value, std::int32_t gameTicks,
    std::int32_t referenceTicks = kFacialReference60HzTicks);
float TimeCorrectedAlpha(float referenceAlpha, std::int32_t gameTicks,
                         std::int32_t referenceTicks =
                             kFacialReference60HzTicks);

// XIII-2 can keep feeding facial animation a 29.97 Hz task delta above 30 FPS.
// Use the frame-pacing wall delta for facial interpolation only, without changing the game's task timing
std::int32_t ResolveFacialTicks(std::int32_t gameTicks,
                               std::uint32_t wallTicks,
                               bool useWallDelta,
                               std::int32_t referenceTicks);

} // namespace novafix::game::facial_animation_policy
