#pragma once

#include <atomic>
#include <cstdint>

namespace novafix::game::frame_pacing::state {

inline std::atomic_bool ownershipActive{};
inline std::atomic_bool highRefreshPresentationActive{};
inline std::atomic_bool gameBoundaryPacingActive{};
inline std::atomic_uint32_t appliedPresentationDeltaTicks{};

} // namespace novafix::game::frame_pacing::state
