#include "game/steam/steam_achievement_queue.h"

#include "diagnostics/log.h"
#include "game/steam/steam_pending_journals.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace novafix::game::steam_cloud::achievement_queue {
namespace {

constexpr std::size_t kMaximumAchievements = 31u;

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<std::string> g_pending;
std::vector<std::string> g_locallyDirty;
std::atomic_bool g_loaded{};
bool g_persistFailureLogged{};

bool ValidIdentifier(std::string_view name) {
    return !name.empty() && name.size() <= 63 &&
           name.rfind("ACV_", 0) == 0;
}

bool Contains(const std::vector<std::string>& values,
              std::string_view name) {
    return std::find(values.begin(), values.end(), name) != values.end();
}

void PersistLocked() {
    if (pending_journals::StoreAchievements(g_pending)) {
        g_persistFailureLogged = false;
    } else if (!g_persistFailureLogged) {
        g_persistFailureLogged = true;
        Log("Unable to persist the pending Steam achievement queue");
    }
}

} // namespace

void LoadOnce() {
    if (g_loaded.exchange(true, std::memory_order_acq_rel)) return;
    AcquireSRWLockExclusive(&g_lock);
    pending_journals::LoadAchievements(g_pending);
    g_pending.erase(
        std::remove_if(g_pending.begin(), g_pending.end(),
                       [](const std::string& name) {
                           return !ValidIdentifier(name);
                       }),
        g_pending.end());
    PersistLocked();
    const std::size_t loaded = g_pending.size();
    ReleaseSRWLockExclusive(&g_lock);
    if (loaded != 0) {
        Log("Loaded %u deferred Steam achievement(s)",
            static_cast<unsigned>(loaded));
    }
}

void Queue(std::string_view name) {
    if (!ValidIdentifier(name)) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!Contains(g_pending, name)) {
        if (g_pending.size() >= kMaximumAchievements) {
            LogWarning(
                "Steam achievement queue capacity reached: rejected=%.*s",
                static_cast<int>(name.size()), name.data());
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        g_pending.emplace_back(name);
        PersistLocked();
        Log("Steam achievement queued: %.*s",
            static_cast<int>(name.size()), name.data());
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::vector<std::string> Snapshot() {
    AcquireSRWLockShared(&g_lock);
    const std::vector<std::string> pending = g_pending;
    ReleaseSRWLockShared(&g_lock);
    return pending;
}

void Complete(const std::vector<std::string>& names) {
    if (names.empty()) return;
    AcquireSRWLockExclusive(&g_lock);
    const auto before = g_pending.size();
    for (const std::string& name : names) {
        std::erase(g_pending, name);
        std::erase(g_locallyDirty, name);
    }
    if (g_pending.size() != before) PersistLocked();
    ReleaseSRWLockExclusive(&g_lock);
}

void MarkDirty(std::string_view name) {
    AcquireSRWLockExclusive(&g_lock);
    if (!Contains(g_locallyDirty, name)) {
        g_locallyDirty.emplace_back(name);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool IsDirty(std::string_view name) {
    AcquireSRWLockShared(&g_lock);
    const bool dirty = Contains(g_locallyDirty, name);
    ReleaseSRWLockShared(&g_lock);
    return dirty;
}

bool HasPending() {
    AcquireSRWLockShared(&g_lock);
    const bool pending = !g_pending.empty();
    ReleaseSRWLockShared(&g_lock);
    return pending;
}

} // namespace novafix::game::steam_cloud::achievement_queue
