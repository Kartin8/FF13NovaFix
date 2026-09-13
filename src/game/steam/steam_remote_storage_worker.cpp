#include "game/steam/steam_remote_storage_worker.h"

#include "common/guarded_call.h"
#include "diagnostics/log.h"
#include "game/steam/steam_pending_journals.h"
#include "game/steam/steam_remote_storage_policy.h"
#include "game/steam/steam_save_identity.h"
#include "game/steam/steam_save_mirror.h"
#include "game/steam/steam_save_reconciliation_policy.h"
#include "game/steam/steam_save_sync_state.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace novafix::game::steam_cloud::remote_storage::worker {
namespace {

constexpr std::int32_t kMaximumRemoteSaveBytes = 100 * 1024 * 1024;

struct PendingWrite {
    enum class Operation {
        Write,
        Delete,
        ReconcileRemote,
    } operation{Operation::Write};
    enum class Origin {
        Reconciliation,
        AuthoritativeLocal,
    } origin{Origin::Reconciliation};
    std::string name;
    std::vector<unsigned char> bytes;
};

SRWLOCK g_queueLock = SRWLOCK_INIT;
std::vector<PendingWrite> g_queue;
HANDLE g_wakeEvent{};
HANDLE g_stopEvent{};
HANDLE g_reconciliationEvent{};
HANDLE g_thread{};
std::atomic_bool g_stopping{};
std::atomic_bool g_asyncEnabled{};
std::atomic_bool g_cloudEnabled{};
std::atomic<Interface*> g_remote{};
std::vector<std::string> g_pendingDeletions;
std::vector<std::string> g_pendingUploads;
std::vector<std::string> g_remotePreservedNames;
std::vector<sync_state::Entry> g_synchronizedSaves;
std::atomic_bool g_pendingOperationsLoaded{};
std::atomic_bool g_remoteReconciliationQueued{};
std::atomic_bool g_reconciliationPending{};
std::atomic_bool g_reconciliationImportAllowed{true};
bool g_deletionPersistFailureLogged{};
bool g_uploadPersistFailureLogged{};
bool g_syncStateLoadFailureLogged{};
bool g_syncStatePersistFailureLogged{};

bool QueueWrite(const char* name, const void* data, std::int32_t size,
                PendingWrite::Origin origin);
bool RecordLocalWriteWithOrigin(
    const char* name, const void* data, std::int32_t size,
    PendingWrite::Origin origin);

bool ContainsName(const std::vector<std::string>& names,
                  const char* candidate) {
    return candidate && std::any_of(
        names.begin(), names.end(), [&](const std::string& name) {
            return _stricmp(name.c_str(), candidate) == 0;
        });
}

void EraseName(std::vector<std::string>& names, const char* candidate) {
    if (!candidate) return;
    std::erase_if(names, [&](const std::string& name) {
        return _stricmp(name.c_str(), candidate) == 0;
    });
}

void PersistPendingDeletions() {
    if (pending_journals::StoreDeletions(g_pendingDeletions)) {
        g_deletionPersistFailureLogged = false;
    } else if (!g_deletionPersistFailureLogged) {
        g_deletionPersistFailureLogged = true;
        LogWarning("Steam Cloud deletion retry queue could not be persisted");
    }
}

void PersistPendingUploads() {
    if (pending_journals::StoreUploads(g_pendingUploads)) {
        g_uploadPersistFailureLogged = false;
    } else if (!g_uploadPersistFailureLogged) {
        g_uploadPersistFailureLogged = true;
        LogWarning("Steam Cloud upload retry queue could not be persisted");
    }
}

void PersistSynchronizedSavesLocked() {
    if (sync_state::Store(g_synchronizedSaves)) {
        g_syncStatePersistFailureLogged = false;
    } else if (!g_syncStatePersistFailureLogged) {
        g_syncStatePersistFailureLogged = true;
        LogWarning("Steam Cloud synchronization state could not be persisted");
    }
}

std::optional<save_identity::ContentIdentity> SynchronizedIdentity(
    const char* name) {
    AcquireSRWLockShared(&g_queueLock);
    const auto found = std::find_if(
        g_synchronizedSaves.begin(), g_synchronizedSaves.end(),
        [&](const sync_state::Entry& entry) {
            return name && _stricmp(entry.name.c_str(), name) == 0;
        });
    const std::optional<save_identity::ContentIdentity> result =
        found == g_synchronizedSaves.end()
            ? std::nullopt
            : std::optional(found->identity);
    ReleaseSRWLockShared(&g_queueLock);
    return result;
}

void RememberSynchronized(
    const char* name,
    const save_identity::ContentIdentity& identity) {
    if (!name) return;
    AcquireSRWLockExclusive(&g_queueLock);
    const auto found = std::find_if(
        g_synchronizedSaves.begin(), g_synchronizedSaves.end(),
        [&](const sync_state::Entry& entry) {
            return _stricmp(entry.name.c_str(), name) == 0;
        });
    if (found == g_synchronizedSaves.end()) {
        g_synchronizedSaves.push_back({name, identity});
    } else {
        found->identity = identity;
    }
    PersistSynchronizedSavesLocked();
    ReleaseSRWLockExclusive(&g_queueLock);
}

void ForgetSynchronized(const char* name) {
    AcquireSRWLockExclusive(&g_queueLock);
    const std::size_t previous = g_synchronizedSaves.size();
    std::erase_if(g_synchronizedSaves,
                  [&](const sync_state::Entry& entry) {
                      return name &&
                          _stricmp(entry.name.c_str(), name) == 0;
                  });
    if (g_synchronizedSaves.size() != previous) {
        PersistSynchronizedSavesLocked();
    }
    ReleaseSRWLockExclusive(&g_queueLock);
}

bool IsUploadPending(const char* name) {
    AcquireSRWLockShared(&g_queueLock);
    const bool pending = ContainsName(g_pendingUploads, name);
    ReleaseSRWLockShared(&g_queueLock);
    return pending;
}

template <typename Result, typename Function>
Result Guarded(Function function, Result fallback) {
    return guarded_call::ResultOr<Result>(std::move(function),
                                          std::move(fallback));
}

bool RemoteCloudEnabled(Interface* remote) {
    return remote && Guarded<bool>([&] {
        return remote->IsCloudEnabledForAccount() &&
               remote->IsCloudEnabledForApp();
    }, false);
}

bool PreserveRemoteVersion(Interface* remote, const PendingWrite& write) {
    if (!remote || write.operation ==
                       PendingWrite::Operation::ReconcileRemote) {
        return remote != nullptr;
    }
    const bool alreadyPreserved = ContainsName(
        g_remotePreservedNames, write.name.c_str());
    if (alreadyPreserved) return true;

    std::int32_t size{};
    if (!guarded_call::Run([&] {
            size = remote->GetFileSize(write.name.c_str());
        })) {
        return false;
    }
    const std::int64_t timestamp = Guarded<std::int64_t>(
        [&] { return remote->GetFileTimestamp(write.name.c_str()); }, 0);
    if (size < 0) return false;
    if (size == 0) {
        g_remotePreservedNames.push_back(write.name);
        return true;
    }
    if (size > kMaximumRemoteSaveBytes) return false;

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    std::int32_t read{};
    if (!guarded_call::Run([&] {
            read = remote->FileRead(write.name.c_str(), bytes.data(), size);
        }) || read != size) {
        return false;
    }
    if (write.operation == PendingWrite::Operation::Write &&
        bytes == write.bytes) {
        g_remotePreservedNames.push_back(write.name);
        return true;
    }
    if (mirror::WriteCloudConflict(
            write.name.c_str(), bytes.data(), size, timestamp)) {
        g_remotePreservedNames.push_back(write.name);
        LogWarning(
            "Steam Cloud differing remote version preserved before %s: %s bytes=%d timestamp=%lld",
            write.operation == PendingWrite::Operation::Write
                ? "overwrite" : "delete",
            write.name.c_str(), size,
            static_cast<long long>(timestamp));
        return true;
    }

    LogWarning(
        "Steam Cloud remote version could not be preserved before %s: %s",
        write.operation == PendingWrite::Operation::Write
            ? "overwrite" : "delete",
        write.name.c_str());
    return false;
}

bool IsDeletionPendingLocked(const char* name) {
    return ContainsName(g_pendingDeletions, name);
}

void ReconcileRemoteSaves(Interface* remote) {
    if (!remote) return;
    const std::int32_t count = Guarded<std::int32_t>(
        [&] { return remote->GetFileCount(); }, 0);
    for (std::int32_t index = 0;
         index < count && index < 256; ++index) {
        std::int32_t size{};
        const char* remoteName = Guarded<const char*>([&] {
            return remote->GetFileNameAndSize(index, &size);
        }, nullptr);
        if (!mirror::IsSafeSaveName(remoteName) ||
            size > kMaximumRemoteSaveBytes) {
            continue;
        }
        const std::string name(remoteName);
        const std::int64_t timestamp = Guarded<std::int64_t>(
            [&] { return remote->GetFileTimestamp(name.c_str()); }, 0);
        if (!policy::IsUsableRemoteCatalogEntry(size, timestamp) ||
            IsDeletionPending(name.c_str())) {
            continue;
        }
        std::vector<unsigned char> remoteBytes(
            static_cast<std::size_t>(size));
        const std::int32_t read = Guarded<std::int32_t>([&] {
            return remote->FileRead(name.c_str(), remoteBytes.data(), size);
        }, 0);
        if (read != size) continue;

        save_identity::ContentIdentity remoteIdentity;
        if (!save_identity::Compute(remoteBytes, remoteIdentity)) {
            LogWarning(
                "Steam Cloud remote save identity failed: %s bytes=%d",
                name.c_str(), size);
            continue;
        }

        std::vector<unsigned char> localBytes;
        const bool localExists = mirror::Exists(name.c_str());
        if (localExists && !mirror::ReadBytes(name.c_str(), localBytes)) {
            LogWarning(
                "Steam Cloud local save could not be read during reconciliation: %s",
                name.c_str());
            continue;
        }

        reconciliation_policy::Version localVersion{
            localExists, {}, localExists ? mirror::Timestamp(name.c_str()) : 0};
        if (localExists &&
            !save_identity::Compute(localBytes, localVersion.identity)) {
            LogWarning(
                "Steam Cloud local save identity failed: %s bytes=%u",
                name.c_str(),
                static_cast<unsigned>(localBytes.size()));
            continue;
        }
        const reconciliation_policy::Version remoteVersion{
            true, remoteIdentity, timestamp};
        const auto synchronized = SynchronizedIdentity(name.c_str());
        const reconciliation_policy::Action action =
            reconciliation_policy::Choose(
                localVersion, remoteVersion,
                synchronized ? &*synchronized : nullptr,
                IsUploadPending(name.c_str()));

        switch (action) {
        case reconciliation_policy::Action::None:
            break;
        case reconciliation_policy::Action::RememberAligned:
            RememberSynchronized(name.c_str(), remoteIdentity);
            break;
        case reconciliation_policy::Action::ImportRemote: {
            if (!g_reconciliationImportAllowed.load(
                    std::memory_order_acquire)) {
                LogWarning(
                    "Steam Cloud remote save changed after the startup reconciliation window closed: %s remote left untouched",
                    name.c_str());
                break;
            }
            const mirror::ConditionalWriteResult imported = localExists
                ? mirror::ReplaceIfUnchanged(
                      name.c_str(), localBytes, remoteBytes)
                : (mirror::ImportCloudSaveIfMissing(
                       name.c_str(), remoteBytes.data(), size, timestamp)
                       ? mirror::ConditionalWriteResult::Written
                       : mirror::ConditionalWriteResult::Failed);
            if (imported == mirror::ConditionalWriteResult::Changed) {
                LogDebug(
                    "Steam Cloud remote import skipped after a concurrent local save: %s",
                    name.c_str());
                break;
            }
            if (imported != mirror::ConditionalWriteResult::Written) {
                LogWarning(
                    "Steam Cloud newer remote save could not be imported: %s",
                    name.c_str());
                break;
            }
            RememberSynchronized(name.c_str(), remoteIdentity);
            LogInfo(
                "Steam Cloud newer remote save imported safely: %s bytes=%d",
                name.c_str(), size);
            break;
        }
        case reconciliation_policy::Action::UploadLocal:
            RecordLocalWriteWithOrigin(
                name.c_str(), localBytes.data(),
                static_cast<std::int32_t>(localBytes.size()),
                PendingWrite::Origin::Reconciliation);
            LogInfo(
                "Steam Cloud external local save change queued: %s bytes=%u",
                name.c_str(), static_cast<unsigned>(localBytes.size()));
            break;
        case reconciliation_policy::Action::ResolveConflictWithLocal:
            RecordLocalWriteWithOrigin(
                name.c_str(), localBytes.data(),
                static_cast<std::int32_t>(localBytes.size()),
                PendingWrite::Origin::Reconciliation);
            LogWarning(
                "Steam Cloud save conflict detected: %s local kept and remote preservation queued",
                name.c_str());
            break;
        }
    }
}

bool RemoteWrite(Interface* remote, const PendingWrite& write) {
    if (!remote || !PreserveRemoteVersion(remote, write)) return false;
    const bool written = Guarded<bool>([&] {
        if (write.operation == PendingWrite::Operation::Delete) {
            return remote->FileDelete(write.name.c_str()) ||
                   !remote->FileExists(write.name.c_str());
        }
        return remote->FileWrite(
            write.name.c_str(), write.bytes.data(),
            static_cast<std::int32_t>(write.bytes.size()));
    }, false);
    if (!written) return false;

    if (write.operation == PendingWrite::Operation::Delete) {
        ForgetSynchronized(write.name.c_str());
    } else if (write.operation == PendingWrite::Operation::Write) {
        save_identity::ContentIdentity identity;
        if (save_identity::Compute(write.bytes, identity)) {
            RememberSynchronized(write.name.c_str(), identity);
        } else {
            LogWarning(
                "Steam Cloud synchronized save identity failed: %s bytes=%u",
                write.name.c_str(),
                static_cast<unsigned>(write.bytes.size()));
        }
    }
    return true;
}

DWORD WINAPI WorkerMain(void*) {
    HANDLE waits[] = {g_stopEvent, g_wakeEvent};
    bool stopping = false;
    for (;;) {
        if (!stopping) {
            const DWORD result =
                WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            stopping = result == WAIT_OBJECT_0;
        }
        PendingWrite write;
        bool found = false;
        AcquireSRWLockExclusive(&g_queueLock);
        if (!g_queue.empty()) {
            write = std::move(g_queue.front());
            g_queue.erase(g_queue.begin());
            found = true;
        }
        ReleaseSRWLockExclusive(&g_queueLock);
        if (found) {
            if (write.operation == PendingWrite::Operation::ReconcileRemote) {
                ReconcileRemoteSaves(
                    g_remote.load(std::memory_order_acquire));
                g_reconciliationPending.store(
                    false, std::memory_order_release);
                SetEvent(g_reconciliationEvent);
                continue;
            }
            if (RemoteWrite(
                    g_remote.load(std::memory_order_acquire), write)) {
                AcquireSRWLockExclusive(&g_queueLock);
                if (write.operation == PendingWrite::Operation::Delete) {
                    EraseName(g_pendingDeletions, write.name.c_str());
                    PersistPendingDeletions();
                } else {
                    const bool superseded = std::any_of(
                        g_queue.begin(), g_queue.end(),
                        [&](const PendingWrite& pending) {
                            return pending.operation ==
                                       PendingWrite::Operation::Write &&
                                   _stricmp(pending.name.c_str(),
                                            write.name.c_str()) == 0;
                        });
                    if (!superseded) {
                        EraseName(g_pendingUploads, write.name.c_str());
                        PersistPendingUploads();
                    }
                }
                ReleaseSRWLockExclusive(&g_queueLock);
            } else {
                LogWarning(
                    "Steam Cloud background write failed: %s bytes=%u local-save=authoritative",
                    write.name.c_str(),
                    static_cast<unsigned>(write.bytes.size()));
            }
            continue;
        }
        if (stopping) break;
    }
    return 0;
}

bool StartWorker() {
    if (g_stopping.load(std::memory_order_acquire)) return false;
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_thread) {
        ReleaseSRWLockExclusive(&g_queueLock);
        return true;
    }
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_reconciliationEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (g_wakeEvent && g_stopEvent && g_reconciliationEvent) {
        g_thread = CreateThread(nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
    }
    if (!g_thread) {
        if (g_wakeEvent) CloseHandle(g_wakeEvent);
        if (g_stopEvent) CloseHandle(g_stopEvent);
        if (g_reconciliationEvent) CloseHandle(g_reconciliationEvent);
        g_wakeEvent = nullptr;
        g_stopEvent = nullptr;
        g_reconciliationEvent = nullptr;
    }
    const bool started = g_thread != nullptr;
    ReleaseSRWLockExclusive(&g_queueLock);
    return started;
}

bool QueueWrite(const char* name, const void* data, std::int32_t size,
                PendingWrite::Origin origin) {
    PendingWrite next{PendingWrite::Operation::Write, origin, name, {}};
    const auto* first = static_cast<const unsigned char*>(data);
    if (size != 0) next.bytes.assign(first, first + size);
    if (!policy::CanQueueRemoteMutation(
            g_asyncEnabled.load(std::memory_order_acquire),
            g_cloudEnabled.load(std::memory_order_acquire))) {
        return false;
    }
    if (!StartWorker()) return false;
    AcquireSRWLockExclusive(&g_queueLock);
    auto existing = std::find_if(
        g_queue.begin(), g_queue.end(), [&](const PendingWrite& item) {
            return _stricmp(item.name.c_str(), name) == 0;
        });
    if (existing == g_queue.end() && g_queue.size() >= 16) {
        ReleaseSRWLockExclusive(&g_queueLock);
        LogWarning(
            "Steam Cloud queue capacity reached: %s remains safe locally and will retry next launch",
            name);
        return false;
    }
    if (existing == g_queue.end()) {
        g_queue.push_back(std::move(next));
    } else if (existing->operation == PendingWrite::Operation::Write &&
               existing->origin == PendingWrite::Origin::AuthoritativeLocal &&
               origin == PendingWrite::Origin::Reconciliation) {
        // Reconciliation sampled local bytes before the game committed its
        // current write. Never let that stale snapshot supersede the live save.
    } else {
        *existing = std::move(next);
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    SetEvent(g_wakeEvent);
    return true;
}

bool QueueDelete(const char* name) {
    PendingWrite next{
        PendingWrite::Operation::Delete,
        PendingWrite::Origin::AuthoritativeLocal, name, {}};
    if (!policy::CanQueueRemoteMutation(
            g_asyncEnabled.load(std::memory_order_acquire),
            g_cloudEnabled.load(std::memory_order_acquire))) {
        return false;
    }
    if (!StartWorker()) return false;
    AcquireSRWLockExclusive(&g_queueLock);
    auto existing = std::find_if(
        g_queue.begin(), g_queue.end(), [&](const PendingWrite& item) {
            return _stricmp(item.name.c_str(), name) == 0;
        });
    if (existing == g_queue.end()) {
        g_queue.push_back(std::move(next));
    } else {
        *existing = std::move(next);
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    SetEvent(g_wakeEvent);
    return true;
}

void QueueRemoteReconciliation() {
    bool expected = false;
    if (!g_remoteReconciliationQueued.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (!policy::CanQueueRemoteMutation(
            g_asyncEnabled.load(std::memory_order_acquire),
            g_cloudEnabled.load(std::memory_order_acquire)) ||
        !StartWorker()) {
        g_remoteReconciliationQueued.store(false, std::memory_order_release);
        return;
    }
    AcquireSRWLockExclusive(&g_queueLock);
    g_reconciliationPending.store(true, std::memory_order_release);
    ResetEvent(g_reconciliationEvent);
    g_queue.insert(g_queue.begin(), {
        PendingWrite::Operation::ReconcileRemote,
        PendingWrite::Origin::Reconciliation, {}, {}});
    ReleaseSRWLockExclusive(&g_queueLock);
    SetEvent(g_wakeEvent);
}

void QueuePersistedOperations() {
    if (!g_pendingOperationsLoaded.load(std::memory_order_acquire) ||
        !policy::CanQueueRemoteMutation(
            g_asyncEnabled.load(std::memory_order_acquire),
            g_cloudEnabled.load(std::memory_order_acquire)) ||
        !g_remote.load(std::memory_order_acquire)) {
        return;
    }

    QueueRemoteReconciliation();

    AcquireSRWLockShared(&g_queueLock);
    const std::vector<std::string> deletions = g_pendingDeletions;
    const std::vector<std::string> uploads = g_pendingUploads;
    ReleaseSRWLockShared(&g_queueLock);
    for (const std::string& name : deletions) {
        if (mirror::IsSafeSaveName(name.c_str())) QueueDelete(name.c_str());
    }
    for (const std::string& name : uploads) {
        if (!mirror::IsSafeSaveName(name.c_str())) continue;
        std::vector<unsigned char> bytes;
        if (mirror::ReadBytes(name.c_str(), bytes)) {
            QueueWrite(
                name.c_str(), bytes.data(),
                static_cast<std::int32_t>(bytes.size()),
                PendingWrite::Origin::AuthoritativeLocal);
        }
    }
}

bool RecordLocalWriteWithOrigin(
    const char* name, const void* data, std::int32_t size,
    PendingWrite::Origin origin) {
    AcquireSRWLockExclusive(&g_queueLock);
    const auto deletionsBefore = g_pendingDeletions.size();
    EraseName(g_pendingDeletions, name);
    std::erase_if(g_queue, [&](const PendingWrite& item) {
        return item.operation == PendingWrite::Operation::Delete &&
               _stricmp(item.name.c_str(), name) == 0;
    });
    if (g_pendingDeletions.size() != deletionsBefore) {
        PersistPendingDeletions();
    }
    if (!ContainsName(g_pendingUploads, name)) {
        g_pendingUploads.emplace_back(name);
        PersistPendingUploads();
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    return QueueWrite(name, data, size, origin);
}

} // namespace

void SetRemote(Interface* remote) {
    g_remote.store(remote, std::memory_order_release);
    g_cloudEnabled.store(
        RemoteCloudEnabled(remote), std::memory_order_release);
    if (!g_pendingOperationsLoaded.exchange(
            true, std::memory_order_acq_rel)) {
        AcquireSRWLockExclusive(&g_queueLock);
        pending_journals::LoadDeletions(g_pendingDeletions);
        pending_journals::LoadUploads(g_pendingUploads);
        if (!sync_state::Load(g_synchronizedSaves)) {
            g_synchronizedSaves.clear();
            if (!g_syncStateLoadFailureLogged) {
                g_syncStateLoadFailureLogged = true;
                LogWarning(
                    "Steam Cloud synchronization state was invalid and will be rebuilt safely");
            }
        }
        const std::size_t deletionCount = g_pendingDeletions.size();
        std::erase_if(g_pendingDeletions, [](const std::string& name) {
            return !mirror::IsSafeSaveName(name.c_str());
        });
        const std::size_t uploadCount = g_pendingUploads.size();
        std::erase_if(g_pendingUploads, [&](const std::string& upload) {
            return !mirror::IsSafeSaveName(upload.c_str()) ||
                   ContainsName(g_pendingDeletions, upload.c_str());
        });
        if (g_pendingDeletions.size() != deletionCount) {
            PersistPendingDeletions();
        }
        if (g_pendingUploads.size() != uploadCount) {
            PersistPendingUploads();
        }
        ReleaseSRWLockExclusive(&g_queueLock);
    }
    QueuePersistedOperations();
}

void ConfigureAsyncWrites(bool enabled) {
    g_asyncEnabled.store(enabled, std::memory_order_release);
    if (enabled) QueuePersistedOperations();
}

bool RecordLocalWrite(const char* name, const void* data, std::int32_t size) {
    return RecordLocalWriteWithOrigin(
        name, data, size, PendingWrite::Origin::AuthoritativeLocal);
}

void RecordLocalDelete(const char* name) {
    AcquireSRWLockExclusive(&g_queueLock);
    const auto uploadsBefore = g_pendingUploads.size();
    EraseName(g_pendingUploads, name);
    std::erase_if(g_queue, [&](const PendingWrite& item) {
        return item.operation == PendingWrite::Operation::Write &&
               _stricmp(item.name.c_str(), name) == 0;
    });
    if (g_pendingUploads.size() != uploadsBefore) {
        PersistPendingUploads();
    }
    if (!ContainsName(g_pendingDeletions, name)) {
        g_pendingDeletions.emplace_back(name);
    }
    PersistPendingDeletions();
    ReleaseSRWLockExclusive(&g_queueLock);
    QueueDelete(name);
}

bool IsDeletionPending(const char* name) {
    AcquireSRWLockShared(&g_queueLock);
    const bool found = IsDeletionPendingLocked(name);
    ReleaseSRWLockShared(&g_queueLock);
    return found;
}

bool CloudEnabled() {
    return g_cloudEnabled.load(std::memory_order_acquire);
}

bool WaitForInitialReconciliation(std::uint32_t milliseconds) {
    if (!g_reconciliationPending.load(std::memory_order_acquire)) {
        return true;
    }
    AcquireSRWLockShared(&g_queueLock);
    HANDLE event{};
    const HANDLE process = GetCurrentProcess();
    if (g_reconciliationEvent) {
        DuplicateHandle(
            process, g_reconciliationEvent, process, &event,
            SYNCHRONIZE, FALSE, 0);
    }
    ReleaseSRWLockShared(&g_queueLock);
    if (!event) {
        g_reconciliationImportAllowed.store(false, std::memory_order_release);
        return false;
    }
    const bool completed =
        WaitForSingleObject(event, milliseconds) == WAIT_OBJECT_0;
    CloseHandle(event);
    if (!completed) {
        // Once the game proceeds with its local catalog, a late background
        // import must not replace the file behind already-loaded state.
        g_reconciliationImportAllowed.store(false, std::memory_order_release);
    }
    return completed;
}

bool Stop(bool wait) {
    g_stopping.store(true, std::memory_order_release);
    AcquireSRWLockShared(&g_queueLock);
    HANDLE thread = g_thread;
    HANDLE stopEvent = g_stopEvent;
    ReleaseSRWLockShared(&g_queueLock);
    if (!thread) return true;
    SetEvent(stopEvent);
    const DWORD result = WaitForSingleObject(thread, wait ? 1000u : 0u);
    if (result != WAIT_OBJECT_0) {
        if (wait) {
            LogWarning(
                "Steam Cloud worker did not drain within 1 second: SteamAPI_Shutdown will be skipped");
        }
        return false;
    }
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_thread == thread) {
        CloseHandle(g_thread);
        CloseHandle(g_wakeEvent);
        CloseHandle(g_stopEvent);
        CloseHandle(g_reconciliationEvent);
        g_thread = nullptr;
        g_wakeEvent = nullptr;
        g_stopEvent = nullptr;
        g_reconciliationEvent = nullptr;
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    return true;
}

} // namespace novafix::game::steam_cloud::remote_storage::worker
