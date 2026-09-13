#include "game/steam/steam_remote_storage_proxy.h"

#include "common/guarded_call.h"
#include "diagnostics/log.h"
#include "game/steam/steam_remote_storage_catalog.h"
#include "game/steam/steam_remote_storage_interface.h"
#include "game/steam/steam_remote_storage_worker.h"
#include "game/steam/steam_save_mirror.h"

#include <atomic>
#include <cstdint>
#include <utility>

namespace novafix::game::steam_cloud::remote_storage {
namespace {

constexpr std::uint32_t kInitialReconciliationWaitMilliseconds = 1000;
std::atomic_bool g_reconciliationTimeoutLogged{};
std::atomic_bool g_reconciliationWaitAbandoned{};

template <typename Result, typename Function>
Result Guarded(Function function, Result fallback) {
    return guarded_call::ResultOr<Result>(std::move(function),
                                          std::move(fallback));
}

void AwaitInitialReconciliation() {
    if (g_reconciliationWaitAbandoned.load(std::memory_order_acquire)) {
        return;
    }
    if (worker::WaitForInitialReconciliation(
            kInitialReconciliationWaitMilliseconds)) {
        return;
    }
    g_reconciliationWaitAbandoned.store(true, std::memory_order_release);
    if (!g_reconciliationTimeoutLogged.exchange(
            true, std::memory_order_acq_rel)) {
        LogWarning(
            "Steam Cloud initial reconciliation exceeded 1 second: local save remains available and remote changes will be preserved before upload");
    }
}

class Proxy final : public Interface {
public:
    void SetRemote(Interface* remote) {
        remote_ = remote;
        worker::SetRemote(remote);
    }

    bool FileWrite(const char* name, const void* data,
                   std::int32_t size) override {
        if (!mirror::IsSafeSaveName(name) || size < 0 ||
            (size && !data)) {
            return false;
        }
        AwaitInitialReconciliation();
        if (!mirror::Write(name, data, size)) {
            LogError("Steam Cloud local-first write failed: %s bytes=%d",
                     name, size);
            return false;
        }
        const bool queued = worker::RecordLocalWrite(name, data, size);
        Log("Steam Cloud save committed locally: %s bytes=%d remote=%s",
            name, size, queued ? "queued" : "unavailable");
        return true;
    }

    std::int32_t FileRead(const char* name, void* data,
                          std::int32_t capacity) override {
        if (!mirror::IsSafeSaveName(name) ||
            worker::IsDeletionPending(name) || !data || capacity < 0) {
            return 0;
        }
        AwaitInitialReconciliation();
        return mirror::Read(name, data, capacity);
    }

    bool FileForget(const char* name) override {
        return mirror::IsSafeSaveName(name);
    }

    bool FileDelete(const char* name) override {
        if (!mirror::IsSafeSaveName(name)) return false;
        AwaitInitialReconciliation();
        const bool local = mirror::Remove(name);
        worker::RecordLocalDelete(name);
        return local;
    }

    SteamApiCall FileShare(const char*) override {
        return 0u;
    }

    bool SetSyncPlatforms(const char* name, std::uint32_t) override {
        return mirror::IsSafeSaveName(name);
    }

    UGCFileWriteStreamHandle FileWriteStreamOpen(const char* name) override {
        return worker::CloudEnabled() && remote_
            ? Guarded<UGCFileWriteStreamHandle>(
                  [&] { return remote_->FileWriteStreamOpen(name); }, 0)
            : 0;
    }

    bool FileWriteStreamWriteChunk(UGCFileWriteStreamHandle handle,
                                   const void* data,
                                   std::int32_t size) override {
        return worker::CloudEnabled() && remote_ &&
               Guarded<bool>([&] {
                   return remote_->FileWriteStreamWriteChunk(
                       handle, data, size);
               }, false);
    }

    bool FileWriteStreamClose(UGCFileWriteStreamHandle handle) override {
        return worker::CloudEnabled() && remote_ &&
               Guarded<bool>(
                   [&] { return remote_->FileWriteStreamClose(handle); },
                   false);
    }

    bool FileWriteStreamCancel(UGCFileWriteStreamHandle handle) override {
        return worker::CloudEnabled() && remote_ &&
               Guarded<bool>(
                   [&] { return remote_->FileWriteStreamCancel(handle); },
                   false);
    }

    bool FileExists(const char* name) override {
        if (worker::IsDeletionPending(name)) return false;
        AwaitInitialReconciliation();
        return mirror::Exists(name);
    }

    bool FilePersisted(const char* name) override {
        return FileExists(name);
    }

    std::int32_t GetFileSize(const char* name) override {
        if (worker::IsDeletionPending(name)) return 0;
        AwaitInitialReconciliation();
        return mirror::Size(name);
    }

    std::int64_t GetFileTimestamp(const char* name) override {
        if (worker::IsDeletionPending(name)) return 0;
        AwaitInitialReconciliation();
        return mirror::Timestamp(name);
    }

    std::uint32_t GetSyncPlatforms(const char* name) override {
        return FileExists(name) ? UINT32_MAX : 0u;
    }

    std::int32_t GetFileCount() override {
        AwaitInitialReconciliation();
        return catalog::Count();
    }

    const char* GetFileNameAndSize(std::int32_t index,
                                   std::int32_t* size) override {
        AwaitInitialReconciliation();
        return catalog::NameAndSize(index, size);
    }

    bool GetQuota(std::int32_t* total, std::int32_t* available) override {
        if (!total || !available) return false;
        *total = 100 * 1024 * 1024;
        *available = *total;
        return true;
    }

    bool IsCloudEnabledForAccount() override {
        return worker::CloudEnabled();
    }

    bool IsCloudEnabledForApp() override {
        return worker::CloudEnabled();
    }

private:
    Interface* remote_{};
};

Proxy g_proxy;

} // namespace

void* Wrap(void* original) {
    if (!original) return nullptr;
    g_proxy.SetRemote(static_cast<Interface*>(original));
    return &g_proxy;
}

void ConfigureAsyncWrites(bool enabled) {
    worker::ConfigureAsyncWrites(enabled);
}

bool StopWorker(bool wait) {
    return worker::Stop(wait);
}

} // namespace novafix::game::steam_cloud::remote_storage
