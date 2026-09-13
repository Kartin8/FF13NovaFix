#pragma once

#include "game/steam/steam_save_identity.h"

#include <cstdint>

namespace novafix::game::steam_cloud::reconciliation_policy {

using save_identity::ContentIdentity;

struct Version {
    bool exists{};
    ContentIdentity identity{};
    std::int64_t timestamp{};
};

enum class Action {
    None,
    RememberAligned,
    ImportRemote,
    UploadLocal,
    ResolveConflictWithLocal,
};

constexpr std::int64_t kTimestampSlackSeconds = 2;

constexpr bool NewerBy(std::int64_t candidate,
                       std::int64_t existing) {
    return candidate > existing && existing >= 0 &&
        static_cast<std::uint64_t>(candidate) -
            static_cast<std::uint64_t>(existing) >
        static_cast<std::uint64_t>(kTimestampSlackSeconds);
}

constexpr Action Choose(
    const Version& local,
    const Version& remote,
    const ContentIdentity* synchronized,
    bool localUploadPending) {
    if (!remote.exists) return Action::None;
    if (!local.exists) return Action::ImportRemote;
    if (local.identity == remote.identity) return Action::RememberAligned;
    if (localUploadPending) return Action::UploadLocal;

    if (synchronized) {
        const bool localChanged = local.identity != *synchronized;
        const bool remoteChanged = remote.identity != *synchronized;
        if (!localChanged && remoteChanged) return Action::ImportRemote;
        if (localChanged && !remoteChanged) return Action::UploadLocal;
    }

    if (NewerBy(remote.timestamp, local.timestamp)) {
        return Action::ImportRemote;
    }
    if (NewerBy(local.timestamp, remote.timestamp)) {
        return Action::UploadLocal;
    }
    return Action::ResolveConflictWithLocal;
}

} // namespace novafix::game::steam_cloud::reconciliation_policy
