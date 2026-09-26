#pragma once

#include <cstdint>

namespace novafix::game::steam_cloud::achievement_policy {

enum class CandidateAction {
    Defer,
    Complete,
    Set,
    Store,
};

constexpr CandidateAction NextAction(bool locallyDirty, bool querySucceeded,
                                     bool unlocked) {
    if (locallyDirty) return CandidateAction::Store;
    if (!querySucceeded) return CandidateAction::Defer;
    return unlocked ? CandidateAction::Complete : CandidateAction::Set;
}

constexpr std::uint64_t RetryDelayMilliseconds(bool storeWasAttempted) {
    // Steam rate-limits StoreStats and recommends calls on the order of minutes.
    // Retry interface/readiness failures quickly, but back off after StoreStats itself is rejected.
    return storeWasAttempted ? 60'000ULL : 1'000ULL;
}

} // namespace novafix::game::steam_cloud::achievement_policy
