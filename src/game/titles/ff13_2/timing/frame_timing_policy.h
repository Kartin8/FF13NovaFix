#pragma once

#include <cstdint>

namespace novafix::game::xiii2_frame_timing_policy {

// Keep GameMain's original cut/effect clock. Only remove the SwapManager wait
float Xiii2FrameSourceRate(float requestedRate, std::int32_t gameMode,
                           bool enabled, std::uint32_t configuredLimit);

// Decouple only known UI (59.94 Hz), gameplay and movie (29.97 Hz) modes.
// Unknown modes keep the native SwapManager wait.
bool Xiii2CanDecouplePresentation(std::int32_t gameMode);

// Convert QPC frame time to 300000 ticks/sec instead of using the fixed 29.97 Hz ring
std::uint32_t Xiii2WallDeltaTicks(std::uint64_t elapsedCounter,
                                  std::uint64_t counterFrequency);

// Use measured frame time only for the render-side m_remenu_con effect
float Xiii2VeffDeltaMilliseconds(float taskDeltaMilliseconds,
                                 std::uint32_t wallTicks,
                                 bool highRefreshActive);

// Replace UPDATE_ROOT_MOVE's fixed 29.97 Hz step with presentation time
// Leave unrelated task-local tick values alone
std::uint32_t Xiii2RootMotionTicks(std::uint32_t taskTicks,
                                  std::uint32_t wallTicks,
                                  bool highRefreshActive,
                                  bool ownsAuthoredQuantum);

} // namespace novafix::game::xiii2_frame_timing_policy
