#pragma once

#include <cstdint>

namespace novafix::game::steam_cloud::remote_storage::policy {

enum class ReadSource {
    None,
    Local,
    Remote,
};

constexpr ReadSource ChooseReadSource(
    std::int32_t localSize, bool cloudEnabled,
    std::int32_t remoteSize) noexcept {
    if (localSize > 0) return ReadSource::Local;
    if (cloudEnabled && remoteSize > 0) return ReadSource::Remote;
    return ReadSource::None;
}

constexpr bool CanQueueRemoteMutation(
    bool asynchronousWritesAvailable, bool cloudEnabled) noexcept {
    return asynchronousWritesAvailable && cloudEnabled;
}

constexpr bool IsUsableRemoteCatalogEntry(
    std::int32_t size, std::int64_t timestamp) noexcept {
    return size > 0 && timestamp > 0;
}

static_assert(ChooseReadSource(1, true, 1) == ReadSource::Local);
static_assert(ChooseReadSource(1, false, 0) == ReadSource::Local);
static_assert(ChooseReadSource(0, true, 1) == ReadSource::Remote);
static_assert(ChooseReadSource(0, false, 1) == ReadSource::None);
static_assert(!CanQueueRemoteMutation(false, true));
static_assert(!CanQueueRemoteMutation(true, false));
static_assert(CanQueueRemoteMutation(true, true));
static_assert(!IsUsableRemoteCatalogEntry(0, 1));
static_assert(!IsUsableRemoteCatalogEntry(1, 0));
static_assert(IsUsableRemoteCatalogEntry(1, 1));

} // namespace novafix::game::steam_cloud::remote_storage::policy
