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
    // Steam documents StoreStats as rate-limited and recommends calls on the
    // order of minutes. Interface/readiness failures can be retried quickly;
    // a rejected StoreStats call must not be hammered every frame or second
    return storeWasAttempted ? 60'000ULL : 1'000ULL;
}

} // namespace novafix::game::steam_cloud::achievement_policy
