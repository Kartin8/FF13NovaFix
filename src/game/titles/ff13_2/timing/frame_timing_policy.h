#pragma once

#include <cstdint>

namespace novafix::game::xiii2_frame_timing_policy {

// Preserve GameMain's authored cut/effect clock; unlock only the SwapManager wait
float Xiii2FrameSourceRate(float requestedRate, std::int32_t gameMode,
                           bool enabled, std::uint32_t configuredLimit);

// Decouple known UI (59.94 Hz), gameplay and movie (29.97 Hz) modes only
// Unknown modes retain the native SwapManager wait
bool Xiii2CanDecouplePresentation(std::int32_t gameMode);

// Convert QPC presentation time to 300000 ticks/second, bypassing the 29.97-Hz ring
std::uint32_t Xiii2WallDeltaTicks(std::uint64_t elapsedCounter,
                                  std::uint64_t counterFrequency);

// Use measured wall time only for the render-side m_remenu_con effect
float Xiii2VeffDeltaMilliseconds(float taskDeltaMilliseconds,
                                 std::uint32_t wallTicks,
                                 bool highRefreshActive);

// Replace UPDATE_ROOT_MOVE's owned 29.97-Hz quantum with presentation time
// Leave unrelated task-local tick values unchanged
std::uint32_t Xiii2RootMotionTicks(std::uint32_t taskTicks,
                                  std::uint32_t wallTicks,
                                  bool highRefreshActive,
                                  bool ownsAuthoredQuantum);

} // namespace novafix::game::xiii2_frame_timing_policy
