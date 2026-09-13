#include "game/steam/steam_shutdown_guard.h"

#include "common/guarded_call.h"
#include "diagnostics/log.h"
#include "game/steam/steam_remote_storage_proxy.h"

#include <atomic>

namespace novafix::game::steam_cloud::shutdown_guard {
namespace {

SteamShutdownFn g_original{};
void* g_statsReceivedCallback{};
std::atomic_bool g_requested{};
std::atomic_bool g_completed{};

void CompleteDeferred() {
    if (!g_requested.load(std::memory_order_acquire) ||
        g_completed.load(std::memory_order_acquire) || !g_original ||
        !remote_storage::StopWorker(false)) {
        return;
    }
    if (g_completed.exchange(true, std::memory_order_acq_rel)) return;
    if (!guarded_call::Run([&] { g_original(); })) {
        Log("SteamAPI_Shutdown raised an exception after callback teardown: "
            "process shutdown continues safely");
    }
}

} // namespace

void Configure(SteamShutdownFn original) {
    g_original = original;
}

void TrackStatsReceivedCallback(void* callback) {
    g_statsReceivedCallback = callback;
}

void CallbackUnregistered(void* callback) {
    if (callback != g_statsReceivedCallback) return;
    g_statsReceivedCallback = nullptr;
    CompleteDeferred();
}

void __cdecl ProtectedShutdown() {
    if (!remote_storage::StopWorker(true)) {
        Log("SteamAPI_Shutdown skipped because the cloud worker is still "
            "inside Steam: local saves and the retry queue are durable");
    }
    g_requested.store(true, std::memory_order_release);
    // Retail sgw.dll calls SteamAPI_Shutdown before unregistering its three
    // callbacks. Defer the real call until its final (1101) callback has been
    // unregistered. If the callback contract was unavailable, preserve the
    // original behavior rather than leaking a live Steam API session
    if (!g_statsReceivedCallback) CompleteDeferred();
}

} // namespace novafix::game::steam_cloud::shutdown_guard
