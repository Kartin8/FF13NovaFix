#include "game/shared/timing/frame_pacing.h"

#include "game/shared/timing/frame_pacing_state.h"

namespace novafix::game::frame_pacing {

bool IsActive() {
    return state::ownershipActive.load(std::memory_order_acquire);
}

bool IsHighRefreshPresentationActive() {
    return state::highRefreshPresentationActive.load(
        std::memory_order_acquire);
}

bool UsesGameBoundaryPacing() {
    return state::gameBoundaryPacingActive.load(
        std::memory_order_acquire);
}

std::uint32_t AppliedPresentationDeltaTicks() {
    return state::appliedPresentationDeltaTicks.load(
        std::memory_order_acquire);
}

} // namespace novafix::game::frame_pacing
