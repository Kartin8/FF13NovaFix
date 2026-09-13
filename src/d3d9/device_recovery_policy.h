#pragma once

namespace novafix::d3d9::device_recovery {

// Unlike XIII-2, XIII keeps issuing work after DEVICELOST/DEVICENOTRESET
// Suspend NovaFix work but keep forwarding Present/Reset so the game can recover
constexpr bool SuspendNovaFixWork(bool conservativeRecovery,
                                  long cooperativeResult) noexcept {
    return conservativeRecovery && cooperativeResult != 0;
}

} // namespace novafix::d3d9::device_recovery
