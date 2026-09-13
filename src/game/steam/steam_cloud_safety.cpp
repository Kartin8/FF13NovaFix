#include "game/steam/steam_cloud_safety.h"

#include "common/guarded_call.h"
#include "diagnostics/log.h"
#include "game/steam/steam_achievement_queue.h"
#include "game/steam/steam_achievement_policy.h"
#include "game/steam/steam_remote_storage_proxy.h"
#include "game/steam/steam_shutdown_guard.h"
#include "patch/iat_hook.h"
#include "patch/pointer_hook.h"
#include "patch/registry.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace novafix::game::steam_cloud {
namespace {

constexpr std::uint32_t kRetailAchievementCount = 32;
constexpr std::size_t kAchievementStride = 396;
constexpr std::size_t kAchievementIdentifierOffset = 4;
constexpr std::size_t kAchievementStateOffset = 392;
constexpr std::size_t kStatsCountOffset = 8;
constexpr std::size_t kStatsTableOffset = 12;
constexpr std::size_t kStatsReadyOffset = 16;
constexpr std::size_t kStatsReceivedCallbackOffset = 20;
constexpr std::size_t kStatsStoredCallbackOffset = 40;
constexpr std::size_t kAchievementStoredCallbackOffset = 60;
constexpr std::size_t kCallbackFlagsOffset = 4;
constexpr unsigned char kCallbackRegistered = 1;
constexpr int kUserStatsStoredCallback = 1102;
constexpr int kUserAchievementStoredCallback = 1103;

std::atomic_bool g_active{};
std::atomic_bool g_terminal{};
std::atomic_flag g_installing = ATOMIC_FLAG_INIT;

using SteamRemoteStorageFn = void* (__cdecl*)();
using SteamUserStatsFn = void* (__cdecl*)();
using SteamRegisterCallbackFn = void (__cdecl*)(void*, int);
using SteamUnregisterCallbackFn = void (__cdecl*)(void*);
using SteamRunCallbacksFn = void (__cdecl*)();
using GetAchievementFn = bool (__thiscall*)(void*, const char*, bool*);
using SetAchievementFn = bool (__thiscall*)(void*, const char*);
using StoreStatsFn = bool (__thiscall*)(void*);

SteamRemoteStorageFn g_originalRemoteStorage{};
SteamUserStatsFn g_steamUserStats{};
SteamRegisterCallbackFn g_originalRegisterCallback{};
SteamUnregisterCallbackFn g_originalUnregisterCallback{};
SteamRunCallbacksFn g_originalRunCallbacks{};
void** g_achievementSlot{};
patch::registry::ClaimId g_achievementClaim{};
void* g_hardenedStatsObject{};
std::atomic_uint32_t g_suppressedCallbacks{};
std::atomic_uint64_t g_achievementFailures{};
std::atomic_uint64_t g_nextPumpAt{};
std::atomic_bool g_remoteStorageExceptionActive{};
std::atomic_bool g_callbackRegistrationExceptionActive{};
std::atomic_bool g_callbackUnregistrationExceptionActive{};
std::atomic_bool g_runCallbacksExceptionActive{};
std::atomic_bool g_achievementReactivationFailureLogged{};

ULONGLONG g_nextAchievementRetry{};

struct AchievementContract {
    void* statsObject{};
    unsigned char* state{};
    std::string identifier;
    bool statsReady{};
};

bool AddressInModule(const void* address, HMODULE module, std::size_t bytes = 1) {
    if (!address || !module) return false;
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(base);
    const auto end = begin + nt->OptionalHeader.SizeOfImage;
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= begin && value <= end && bytes <= end - value;
}

bool IsReadableRange(const void* address, std::size_t bytes) {
    if (!address || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    if (!(memory.Protect & readable)) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionEnd =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return begin <= regionEnd && bytes <= regionEnd - begin;
}

bool IsExecutableAddress(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (memory.Protect & executable) != 0;
}

bool ReadBoundedIdentifier(const char* identifier, HMODULE bridge, std::string& output) {
    output.clear();
    if (!AddressInModule(identifier, bridge)) return false;
    for (std::size_t index = 0; index < 64; ++index) {
        if (!AddressInModule(identifier + index, bridge)) return false;
        const unsigned char character = static_cast<unsigned char>(identifier[index]);
        if (character == 0) return !output.empty();
        const bool valid = character == '_' ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9');
        if (!valid) return false;
        output.push_back(static_cast<char>(character));
    }
    return false;
}

void* ValidBridgeObject(void** singleton, HMODULE bridge) {
    if (!IsReadableRange(singleton, sizeof(void*))) return nullptr;
    void* object = *singleton;
    if (!IsReadableRange(object, 12)) return nullptr;
    void** vtable = *reinterpret_cast<void***>(object);
    if (!AddressInModule(vtable, bridge, 12 * sizeof(void*)) ||
        !AddressInModule(vtable[1], bridge)) {
        return nullptr;
    }
    return object;
}

void* StatsObject(void* bridgeObject) {
    if (!IsReadableRange(bridgeObject, 12)) return nullptr;
    void* stats = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(bridgeObject) + 8);
    return IsReadableRange(stats, kAchievementStoredCallbackOffset + 20)
        ? stats : nullptr;
}

bool AchievementForIndex(void* bridgeObject, HMODULE bridge, int index,
                         AchievementContract& contract) {
    contract = {};
    void* stats = StatsObject(bridgeObject);
    if (!stats) return false;

    const auto* bytes = static_cast<const unsigned char*>(stats);
    const auto count = *reinterpret_cast<const std::uint32_t*>(
        bytes + kStatsCountOffset);
    auto* table = *reinterpret_cast<unsigned char* const*>(
        bytes + kStatsTableOffset);
    if (count != kRetailAchievementCount || index <= 0 ||
        static_cast<std::uint32_t>(index) >= count ||
        !AddressInModule(table, bridge, count * kAchievementStride)) {
        return false;
    }

    unsigned char* entry = table + static_cast<std::size_t>(index) *
        kAchievementStride;
    const char* identifier = *reinterpret_cast<const char* const*>(
        entry + kAchievementIdentifierOffset);
    std::string name;
    if (!ReadBoundedIdentifier(identifier, bridge, name) ||
        name.rfind("ACV_", 0) != 0) {
        return false;
    }

    contract.statsObject = stats;
    contract.state = entry + kAchievementStateOffset;
    contract.identifier = std::move(name);
    contract.statsReady = bytes[kStatsReadyOffset] != 0;
    return true;
}

bool AchievementForIdentifier(void* bridgeObject, HMODULE bridge,
                              std::string_view identifier,
                              AchievementContract& contract) {
    for (int index = 1; index < static_cast<int>(kRetailAchievementCount); ++index) {
        AchievementContract candidate;
        if (AchievementForIndex(bridgeObject, bridge, index, candidate) &&
            candidate.identifier == identifier) {
            contract = std::move(candidate);
            return true;
        }
    }
    contract = {};
    return false;
}

void* __cdecl ProtectedRemoteStorage() {
    if (!g_originalRemoteStorage) return nullptr;
    void* original{};
    if (!guarded_call::Run([&] {
        original = g_originalRemoteStorage();
    })) {
        if (!g_remoteStorageExceptionActive.exchange(
                true, std::memory_order_acq_rel)) {
            Log("SteamRemoteStorage raised an exception: sgw.dll will use LOCAL_SAVE "
                "until the accessor recovers");
        }
        return nullptr;
    }
    g_remoteStorageExceptionActive.store(false, std::memory_order_release);
    return remote_storage::Wrap(original);
}

void __cdecl ProtectedRegisterCallback(void* callback, int callbackId) {
    if ((callbackId == kUserStatsStoredCallback ||
         callbackId == kUserAchievementStoredCallback)) {
        g_suppressedCallbacks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_originalRegisterCallback) return;
    if (!guarded_call::Run([&] {
        g_originalRegisterCallback(callback, callbackId);
    })) {
        if (!g_callbackRegistrationExceptionActive.exchange(
                true, std::memory_order_acq_rel)) {
            Log("sgw.dll Steam callback registration %d raised an exception; repeats suppressed until recovery",
                callbackId);
        }
    } else {
        g_callbackRegistrationExceptionActive.store(
            false, std::memory_order_release);
    }
}

void __cdecl ProtectedUnregisterCallback(void* callback) {
    if (g_originalUnregisterCallback) {
        if (!guarded_call::Run([&] {
            g_originalUnregisterCallback(callback);
        })) {
            if (!g_callbackUnregistrationExceptionActive.exchange(
                    true, std::memory_order_acq_rel)) {
                Log("sgw.dll Steam callback unregistration raised an exception; repeats suppressed until recovery");
            }
        } else {
            g_callbackUnregistrationExceptionActive.store(
                false, std::memory_order_release);
        }
    }
    shutdown_guard::CallbackUnregistered(callback);
}

void __cdecl ProtectedRunCallbacks() {
    if (!g_originalRunCallbacks) return;
    if (!guarded_call::Run([&] {
        g_originalRunCallbacks();
    })) {
        g_achievementFailures.fetch_add(1, std::memory_order_relaxed);
        if (!g_runCallbacksExceptionActive.exchange(
                true, std::memory_order_acq_rel)) {
            Log("SteamAPI_RunCallbacks raised an exception inside sgw.dll: "
                "damaged callback dispatch contained; repeats suppressed until recovery");
        }
    } else {
        g_runCallbacksExceptionActive.store(false, std::memory_order_release);
    }
}

bool ResolveSteamStatsApi() {
    if (g_steamUserStats) return true;
    HMODULE steamApi = GetModuleHandleW(L"steam_api.dll");
    if (!steamApi) return false;
    auto* accessor = GetProcAddress(steamApi, "SteamUserStats");
    if (!accessor) return false;
    g_steamUserStats = reinterpret_cast<SteamUserStatsFn>(accessor);
    return true;
}

void* CurrentUserStats() {
    if (!ResolveSteamStatsApi()) return nullptr;
    void* stats = guarded_call::ResultOr<void*>(
        [&] { return g_steamUserStats(); }, nullptr);
    if (!IsReadableRange(stats, sizeof(void*))) return nullptr;
    void** vtable = *reinterpret_cast<void***>(stats);
    if (!IsReadableRange(vtable, 11 * sizeof(void*)) ||
        !IsExecutableAddress(vtable[6]) ||
        !IsExecutableAddress(vtable[7]) ||
        !IsExecutableAddress(vtable[10])) {
        return nullptr;
    }
    return stats;
}

bool QueryAchievement(void* stats, const char* identifier, bool& unlocked) {
    unlocked = false;
    if (!stats || !identifier) return false;
    void** vtable = *reinterpret_cast<void***>(stats);
    auto function = reinterpret_cast<GetAchievementFn>(vtable[6]);
    return guarded_call::ResultOr<bool>(
        [&] { return function(stats, identifier, &unlocked); }, false);
}

bool SetAchievement(void* stats, const char* identifier) {
    void** vtable = *reinterpret_cast<void***>(stats);
    auto function = reinterpret_cast<SetAchievementFn>(vtable[7]);
    return guarded_call::ResultOr<bool>(
        [&] { return function(stats, identifier); }, false);
}

bool StoreStats(void* stats) {
    void** vtable = *reinterpret_cast<void***>(stats);
    auto function = reinterpret_cast<StoreStatsFn>(vtable[10]);
    return guarded_call::ResultOr<bool>([&] { return function(stats); }, false);
}

bool FlushPendingAchievements(void* bridgeObject, HMODULE bridge,
                              bool& storeAttempted) {
    storeAttempted = false;
    const std::vector<std::string> pending = achievement_queue::Snapshot();
    if (pending.empty()) return true;

    void* steamStats = CurrentUserStats();
    if (!steamStats) return false;

    std::vector<std::string> completed;
    std::vector<std::string> dirty;
    for (const std::string& name : pending) {
        AchievementContract contract;
        if (!AchievementForIdentifier(bridgeObject, bridge, name, contract) ||
            !contract.statsReady) {
            continue;
        }

        const bool locallyDirty = achievement_queue::IsDirty(name);
        bool unlocked{};
        const bool querySucceeded =
            locallyDirty || QueryAchievement(steamStats, name.c_str(), unlocked);
        using achievement_policy::CandidateAction;
        switch (achievement_policy::NextAction(
            locallyDirty, querySucceeded, unlocked)) {
        case CandidateAction::Complete:
            *contract.state = 1;
            completed.push_back(name);
            break;
        case CandidateAction::Set:
            if (SetAchievement(steamStats, name.c_str())) {
                *contract.state = 1;
                achievement_queue::MarkDirty(name);
                dirty.push_back(name);
            } else {
                g_achievementFailures.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case CandidateAction::Store:
            dirty.push_back(name);
            break;
        case CandidateAction::Defer:
            break;
        }
    }

    if (!dirty.empty()) {
        storeAttempted = true;
        if (StoreStats(steamStats)) {
            completed.insert(completed.end(), dirty.begin(), dirty.end());
        } else {
            g_achievementFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    achievement_queue::Complete(completed);
    for (const std::string& name : completed) {
        Log("Steam achievement committed safely: %s", name.c_str());
    }
    return !completed.empty();
}

void** FindSingletonSlot(HMODULE bridge) {
    auto* initializer = reinterpret_cast<const unsigned char*>(
        GetProcAddress(bridge, "white_steam_init"));
    if (!initializer || !AddressInModule(initializer, bridge, 128)) return nullptr;
    struct Candidate {
        void** slot{};
        unsigned references{};
    } candidates[8]{};
    unsigned candidateCount{};
    for (std::size_t offset = 0; offset + 7 < 128; ++offset) {
        const unsigned char* instruction = initializer + offset;
        const unsigned char* encoded{};
        if (instruction[0] == 0x83 && instruction[1] == 0x3D &&
            instruction[6] == 0x00) {
            encoded = instruction + 2;
        } else if (instruction[0] == 0xA1) {
            encoded = instruction + 1;
        } else if (instruction[0] == 0x8B && instruction[1] == 0x0D) {
            encoded = instruction + 2;
        } else {
            continue;
        }
        std::uint32_t absolute{};
        std::memcpy(&absolute, encoded, sizeof(absolute));
        auto** slot = reinterpret_cast<void**>(
            static_cast<std::uintptr_t>(absolute));
        if (!AddressInModule(slot, bridge, sizeof(void*))) continue;
        auto existing = std::find_if(
            std::begin(candidates), std::begin(candidates) + candidateCount,
            [&](const Candidate& item) { return item.slot == slot; });
        if (existing != std::begin(candidates) + candidateCount) {
            ++existing->references;
        } else if (candidateCount < std::size(candidates)) {
            candidates[candidateCount++] = {slot, 1};
        }
    }
    auto best = std::max_element(
        std::begin(candidates), std::begin(candidates) + candidateCount,
        [](const Candidate& left, const Candidate& right) {
            return left.references < right.references;
        });
    return best != std::begin(candidates) + candidateCount &&
        best->references >= 2 ? best->slot : nullptr;
}

bool BridgeAndContract(HMODULE bridge, void*& bridgeObject) {
    void** singleton = FindSingletonSlot(bridge);
    bridgeObject = singleton ? ValidBridgeObject(singleton, bridge) : nullptr;
    if (!bridgeObject) return false;
    AchievementContract sentinel;
    return AchievementForIndex(bridgeObject, bridge, 1, sentinel) &&
           sentinel.identifier == "ACV_EVENT_00";
}

void HardenNoopCallbacks(void* bridgeObject) {
    void* stats = StatsObject(bridgeObject);
    if (!stats || stats == g_hardenedStatsObject ||
        !g_originalUnregisterCallback) {
        return;
    }

    auto* bytes = static_cast<unsigned char*>(stats);
    void* statsReceived = bytes + kStatsReceivedCallbackOffset;
    auto* statsReceivedFlags = static_cast<unsigned char*>(statsReceived) +
        kCallbackFlagsOffset;
    shutdown_guard::TrackStatsReceivedCallback(
        (*statsReceivedFlags & kCallbackRegistered) != 0
            ? statsReceived : nullptr);
    unsigned detached{};
    for (const std::size_t offset :
         {kStatsStoredCallbackOffset, kAchievementStoredCallbackOffset}) {
        void* callback = bytes + offset;
        auto* flags = static_cast<unsigned char*>(callback) +
            kCallbackFlagsOffset;
        if ((*flags & kCallbackRegistered) == 0) continue;
        if (guarded_call::Run([&] {
            g_originalUnregisterCallback(callback);
        })) {
            ++detached;
        } else {
            Log("Unable to detach sgw.dll no-op Steam callback at +0x%zX", offset);
        }
    }
    g_hardenedStatsObject = stats;
    if (detached != 0) {
        g_suppressedCallbacks.fetch_add(detached, std::memory_order_relaxed);
        Log("sgw.dll no-op post-achievement callbacks detached: %u", detached);
    }
}

bool __fastcall ReliableAchievement(void* self, void*, int index) {
    HMODULE bridge = GetModuleHandleW(L"sgw.dll");
    AchievementContract contract;
    if (!bridge || !AchievementForIndex(self, bridge, index, contract)) {
        g_achievementFailures.fetch_add(1, std::memory_order_relaxed);
        LogWarning("sgw.dll rejected invalid achievement index: %d", index);
        return true;
    }

    achievement_queue::Queue(contract.identifier);
    bool storeAttempted{};
    FlushPendingAchievements(self, bridge, storeAttempted);
    const ULONGLONG delay =
        achievement_policy::RetryDelayMilliseconds(storeAttempted);
    g_nextAchievementRetry = GetTickCount64() + delay;
    // The game treats achievements as one-shot events. Once NovaFix has made
    // the request durable, it must report success even while Steam is offline
    return true;
}

void TryInstallAchievementHook(void* bridgeObject) {
    HardenNoopCallbacks(bridgeObject);
    if (g_achievementSlot) {
        const bool stillOwned =
            *g_achievementSlot == reinterpret_cast<void*>(&ReliableAchievement);
        if (!stillOwned && g_achievementClaim != 0) {
            if (patch::registry::ReactivateVtableClaim(g_achievementClaim)) {
                g_achievementReactivationFailureLogged.store(
                    false, std::memory_order_release);
            } else if (!g_achievementReactivationFailureLogged.exchange(
                           true, std::memory_order_acq_rel)) {
                LogError("sgw.dll achievement hook lost ownership and was not reactivated; repeats suppressed");
            }
        } else if (stillOwned) {
            g_achievementReactivationFailureLogged.store(
                false, std::memory_order_release);
        }
        return;
    }

    void** vtable = *reinterpret_cast<void***>(bridgeObject);
    void* original{};
    if (!patch::InstallPointerHook(
            "sgw:reliable-achievements", &vtable[1],
            reinterpret_cast<void*>(&ReliableAchievement), &original,
            patch::registry::Kind::VtableHook, &g_achievementClaim)) {
        return;
    }
    g_achievementSlot = &vtable[1];
}

void RetryAchievements(HMODULE bridge, void* bridgeObject) {
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextAchievementRetry) return;

    if (!achievement_queue::HasPending()) return;

    bool storeAttempted{};
    FlushPendingAchievements(bridgeObject, bridge, storeAttempted);
    g_nextAchievementRetry = now +
        achievement_policy::RetryDelayMilliseconds(storeAttempted);
}

bool PatchOptionalImport(HMODULE bridge, const char* name, void* replacement,
                         void** original) {
    if (PatchModuleImport(bridge, "sgw.dll", "steam_api.dll", name,
                          replacement, original)) {
        return true;
    }
    LogWarning("sgw.dll optional hardening unavailable: %s import contract mismatch", name);
    return false;
}

void TryInstall() {
    if (g_active.load(std::memory_order_acquire) ||
        g_terminal.load(std::memory_order_acquire) ||
        g_installing.test_and_set(std::memory_order_acquire)) {
        return;
    }
    HMODULE bridge = GetModuleHandleW(L"sgw.dll");
    if (!bridge) {
        g_installing.clear(std::memory_order_release);
        return;
    }

    void* remoteOriginal{};
    if (!PatchModuleImport(
            bridge, "sgw.dll", "steam_api.dll", "SteamRemoteStorage",
            reinterpret_cast<void*>(&ProtectedRemoteStorage), &remoteOriginal)) {
        g_terminal.store(true, std::memory_order_release);
        LogWarning("sgw.dll hardening disabled: SteamRemoteStorage import contract mismatch");
        g_installing.clear(std::memory_order_release);
        return;
    }
    g_originalRemoteStorage =
        reinterpret_cast<SteamRemoteStorageFn>(remoteOriginal);

    void* shutdownOriginal{};
    if (PatchOptionalImport(
            bridge, "SteamAPI_Shutdown",
            reinterpret_cast<void*>(&shutdown_guard::ProtectedShutdown),
            &shutdownOriginal)) {
        shutdown_guard::Configure(
            reinterpret_cast<shutdown_guard::SteamShutdownFn>(
                shutdownOriginal));
        remote_storage::ConfigureAsyncWrites(true);
    } else {
        remote_storage::ConfigureAsyncWrites(false);
    }

    void* registerOriginal{};
    if (PatchOptionalImport(
            bridge, "SteamAPI_RegisterCallback",
            reinterpret_cast<void*>(&ProtectedRegisterCallback), &registerOriginal)) {
        g_originalRegisterCallback =
            reinterpret_cast<SteamRegisterCallbackFn>(registerOriginal);
    }

    void* unregisterOriginal{};
    if (PatchOptionalImport(
            bridge, "SteamAPI_UnregisterCallback",
            reinterpret_cast<void*>(&ProtectedUnregisterCallback),
            &unregisterOriginal)) {
        g_originalUnregisterCallback =
            reinterpret_cast<SteamUnregisterCallbackFn>(unregisterOriginal);
    }

    void* callbacksOriginal{};
    if (PatchOptionalImport(
            bridge, "SteamAPI_RunCallbacks",
            reinterpret_cast<void*>(&ProtectedRunCallbacks), &callbacksOriginal)) {
        g_originalRunCallbacks =
            reinterpret_cast<SteamRunCallbacksFn>(callbacksOriginal);
    }

    achievement_queue::LoadOnce();
    g_active.store(true, std::memory_order_release);
    g_installing.clear(std::memory_order_release);

    // In the usual retail startup the bridge already exists by the first D3D9
    // initialization. Install immediately so the safety path is independent
    // of presentation modes that intentionally bypass the normal feature pump
    void* bridgeObject{};
    if (BridgeAndContract(bridge, bridgeObject)) {
        TryInstallAchievementHook(bridgeObject);
    }
}

} // namespace

void InstallEarly() { TryInstall(); }

bool InstallationViable() {
    return !g_terminal.load(std::memory_order_acquire);
}

void Pump() {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG next = g_nextPumpAt.load(std::memory_order_relaxed);
    if (now < next) return;
    const ULONGLONG interval =
        g_active.load(std::memory_order_acquire) ? 1000u : 250u;
    if (!g_nextPumpAt.compare_exchange_strong(
            next, now + interval, std::memory_order_relaxed)) {
        return;
    }
    InstallEarly();
    if (!g_active.load(std::memory_order_acquire)) return;
    HMODULE bridge = GetModuleHandleW(L"sgw.dll");
    if (!bridge) return;
    void* bridgeObject{};
    if (!BridgeAndContract(bridge, bridgeObject)) return;
    TryInstallAchievementHook(bridgeObject);
    RetryAchievements(bridge, bridgeObject);
}

} // namespace novafix::game::steam_cloud
