#include "game/steam/steam_remote_storage_catalog.h"

#include "game/steam/steam_remote_storage_worker.h"
#include "game/steam/steam_save_mirror.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace novafix::game::steam_cloud::remote_storage::catalog {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<mirror::FileInfo> g_entries;
thread_local std::string g_resultName;

void Refresh() {
    std::vector<mirror::FileInfo> next = mirror::Enumerate();
    std::erase_if(next, [](const mirror::FileInfo& entry) {
        return worker::IsDeletionPending(entry.name.c_str());
    });
    AcquireSRWLockExclusive(&g_lock);
    g_entries.swap(next);
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

std::int32_t Count() {
    Refresh();
    AcquireSRWLockShared(&g_lock);
    const std::int32_t count =
        static_cast<std::int32_t>(g_entries.size());
    ReleaseSRWLockShared(&g_lock);
    return count;
}

const char* NameAndSize(std::int32_t index, std::int32_t* size) {
    if (size) *size = 0;
    AcquireSRWLockShared(&g_lock);
    if (index < 0 ||
        static_cast<std::size_t>(index) >= g_entries.size()) {
        ReleaseSRWLockShared(&g_lock);
        return nullptr;
    }
    const mirror::FileInfo& entry = g_entries[index];
    g_resultName = entry.name;
    if (size) *size = entry.size;
    ReleaseSRWLockShared(&g_lock);
    return g_resultName.c_str();
}

} // namespace novafix::game::steam_cloud::remote_storage::catalog
