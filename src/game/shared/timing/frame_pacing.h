#pragma once

#include <cstdint>

namespace novafix::game::frame_pacing {

bool IsActive();

// Some game tasks keep their original fixed scheduler step even when presentation is uncapped.
// Consumers that read that step directly can use the measured frame delta instead.
bool IsHighRefreshPresentationActive();
// XIII-2 can pace at the game frame boundary instead of Present.
// Only one limiter site should be active at a time.
bool UsesGameBoundaryPacing();
std::uint32_t AppliedPresentationDeltaTicks();

} // namespace novafix::game::frame_pacing
