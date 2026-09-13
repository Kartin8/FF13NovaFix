#pragma once

#include <cstdint>

namespace novafix::game::frame_pacing {

bool IsActive();

// A title runtime may retain an authored scheduler quantum while decoupling
// presentation. Consumers which receive that quantum directly can use the
// measured wall-clock delta exposed here instead
bool IsHighRefreshPresentationActive();
// XIII-2 can move the limiter from Present to the game frame boundary
// This keeps the two limiter sites mutually exclusive
bool UsesGameBoundaryPacing();
std::uint32_t AppliedPresentationDeltaTicks();

} // namespace novafix::game::frame_pacing
