#include "game/titles/ff13/ui/prompt_mode_manager.h"
#include "game/titles/ff13/ui/prompt_layout_registry.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "input/activity.h"
#include "input/prompt_policy.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "patch/signature.h"
#include "settings/input_settings.h"

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::ff13_prompt_mode {
namespace {

// XIII Steam 2014: 8FD5F0 copies keyboard cells; 8FD520 restores the pad atlas
// Switch after the input tick without changing the saved setting
constexpr std::uint32_t kControllerAtlasBackupRva = 0x02426BF0u;
constexpr std::uint32_t kNativePromptFlagRva = 0x02426BF4u;
constexpr std::uint32_t kInputTickRva = 0x0041FBE0u;
constexpr std::uint32_t kSettingGetterRva = 0x00725A10u;
constexpr std::uint32_t kSurfaceUnwrapRva = 0x00AC1F00u;
constexpr std::ptrdiff_t kRestoreControllerOffset = 0x1E0;
constexpr std::ptrdiff_t kReplaceKeyboardOffset = 0x2B0;

constexpr std::array<std::uint8_t, 9> kPromptWrapperPrefix{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 16> kReplaceKeyboardPrefix{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xB4, 0x01, 0x00,
    0x00, 0x56, 0x57, 0x6A, 0x2C, 0x6A, 0x00, 0x8D,
};
constexpr std::array<std::uint8_t, 6> kInputTickPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C,
};
constexpr std::array<std::uint8_t, 14> kSurfaceUnwrapPrefix{
    0x80, 0x3D, 0x60, 0x51, 0x6E, 0x02, 0x00, 0x75,
    0x05, 0x8B, 0x44, 0x24, 0x04, 0xC3,
};
using NativeMode = input::NativePromptMode;
using InputTickFn = int (__thiscall*)(void* manager, const std::int32_t* timing);
using ReplaceKeyboardAtlasFn = char (__cdecl*)(void* texture, char force);
using RestoreControllerAtlasFn = void (__cdecl*)(void* texture);
using PromptWrapperFn = int (__cdecl*)(char forceKeyboard);
using SettingGetterFn = int (__cdecl*)(int settingIndex);
using TextureLevelFn = void* (__thiscall*)(void* texture, int level);
using SurfaceUnwrapFn = IDirect3DSurface9* (__cdecl*)(void* resource);
using D3DXLoadSurfaceFromSurfaceFn = HRESULT (WINAPI*)(
    IDirect3DSurface9* destination, const PALETTEENTRY* destinationPalette,
    const RECT* destinationRectangle, IDirect3DSurface9* source,
    const PALETTEENTRY* sourcePalette, const RECT* sourceRectangle,
    DWORD filter, D3DCOLOR colorKey);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_inputTickDetour;
patch::Detour32 g_promptWrapperDetour;
patch::Detour32 g_replaceKeyboardAtlasDetour;
patch::Detour32 g_restoreControllerAtlasDetour;
InputTickFn g_originalInputTick{};
PromptWrapperFn g_originalPromptWrapper{};
SettingGetterFn g_settingGetter{};
SurfaceUnwrapFn g_surfaceUnwrap{};
D3DXLoadSurfaceFromSurfaceFn g_loadSurfaceFromSurface{};
ReplaceKeyboardAtlasFn g_replaceKeyboardAtlas{};
RestoreControllerAtlasFn g_restoreControllerAtlas{};
void** g_controllerAtlasBackup{};
volatile LONG* g_nativePromptFlag{};
std::atomic_bool g_available{false};
std::atomic_bool g_atlasRecoveryPending{false};
std::atomic_bool g_controllerBackupRefreshPending{false};
std::atomic<IDirect3DSurface9*> g_controllerBackupSnapshot{nullptr};
std::atomic_bool g_controllerBackupCaptureFailureLogged{false};
std::atomic_bool g_controllerBackupRestoreFailureLogged{false};
SRWLOCK g_controllerBackupSnapshotLock = SRWLOCK_INIT;
std::atomic<void*> g_cachedButtonAtlas{nullptr};
NativeMode g_automaticPromptCandidate{NativeMode::Unknown};
ULONGLONG g_automaticPromptCandidateSince{};
SRWLOCK g_atlasMutationLock = SRWLOCK_INIT;
thread_local bool g_nativeWrapperOwnsAtlasLock{};

template <typename T>
bool ReadAt(const void* base, std::size_t offset, T& output) {
    const auto* address = static_cast<const std::byte*>(base) + offset;
    if (!patch::memory::IsReadable(address, sizeof(T))) return false;
    std::memcpy(&output, address, sizeof(T));
    return true;
}

NativeMode ReadActualMode() {
    if (!g_nativePromptFlag) return NativeMode::Unknown;
    return InterlockedCompareExchange(g_nativePromptFlag, 0, 0) == 0
        ? NativeMode::Controller : NativeMode::Keyboard;
}

NativeMode DesiredModeNow() {
    return input::ResolvePromptMode(
        settings::ActiveInput().promptMode,
        input::LatestActivity());
}

const char* ModeName(NativeMode mode) {
    if (mode == NativeMode::Keyboard) return "keyboard";
    if (mode == NativeMode::Controller) return "controller";
    return "unknown";
}

void CacheButtonAtlas(void* atlas) {
    if (!atlas) return;
    (void)g_cachedButtonAtlas.exchange(atlas, std::memory_order_acq_rel);
}

void* CachedButtonAtlas() {
    void* cached = g_cachedButtonAtlas.load(
        std::memory_order_acquire);
    if (cached && patch::memory::IsReadable(cached, sizeof(void*))) {
        return cached;
    }
    if (cached) {
        g_cachedButtonAtlas.compare_exchange_strong(
            cached, nullptr,
            std::memory_order_acq_rel);
    }
    return nullptr;
}

bool AtlasReady() {
    return g_controllerAtlasBackup &&
           patch::memory::IsReadable(
               g_controllerAtlasBackup, sizeof(void*)) &&
           *g_controllerAtlasBackup != nullptr;
}

IDirect3DSurface9* ResolveControllerBackupSurface() {
    if (!AtlasReady() || !g_surfaceUnwrap) {
        return nullptr;
    }

    void* const backup = *g_controllerAtlasBackup;
    void** backupVtable{};
    if (!ReadAt(backup, 0u, backupVtable) || !backupVtable ||
        !patch::memory::IsReadable(backupVtable + 4u, sizeof(void*))) {
        return nullptr;
    }

    TextureLevelFn getBackupLevel{};
    std::memcpy(&getBackupLevel, backupVtable + 4u,
                sizeof(getBackupLevel));
    if (!getBackupLevel ||
        !patch::memory::IsExecutable(
            reinterpret_cast<const void*>(getBackupLevel), 1u)) {
        return nullptr;
    }

    void* const backupLevel = getBackupLevel(backup, 0);
    void* resourceOwner{};
    void* resource{};
    if (!backupLevel || !ReadAt(backupLevel, 48u, resourceOwner) ||
        !resourceOwner || !ReadAt(resourceOwner, 32u, resource) ||
        !resource) {
        return nullptr;
    }

    return g_surfaceUnwrap(resource);
}

bool EnsureControllerBackupSnapshot() {
    if (!g_loadSurfaceFromSurface) {
        return false;
    }
    if (g_controllerBackupSnapshot.load(std::memory_order_acquire)) {
        return true;
    }

    AcquireSRWLockExclusive(&g_controllerBackupSnapshotLock);
    if (g_controllerBackupSnapshot.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&g_controllerBackupSnapshotLock);
        return true;
    }

    IDirect3DSurface9* const source = ResolveControllerBackupSurface();
    if (!source) {
        ReleaseSRWLockExclusive(&g_controllerBackupSnapshotLock);
        return false;
    }

    D3DSURFACE_DESC description{};
    IDirect3DDevice9* device{};
    HRESULT result = source->GetDesc(&description);
    if (SUCCEEDED(result)) {
        result = source->GetDevice(&device);
    }
    IDirect3DSurface9* snapshot{};
    if (SUCCEEDED(result) && device) {
        result = device->CreateOffscreenPlainSurface(
            description.Width, description.Height, description.Format,
            D3DPOOL_SYSTEMMEM, &snapshot, nullptr);
    }
    if (SUCCEEDED(result) && snapshot) {
        result = g_loadSurfaceFromSurface(
            snapshot, nullptr, nullptr, source, nullptr, nullptr,
            3u, 0u);
    }
    if (device) {
        device->Release();
    }
    if (FAILED(result) || !snapshot) {
        if (snapshot) snapshot->Release();
        ReleaseSRWLockExclusive(&g_controllerBackupSnapshotLock);
        if (!g_controllerBackupCaptureFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            LogWarning("XIII pristine controller-atlas snapshot capture failed: result=0x%08lX",
                       static_cast<unsigned long>(result));
        }
        return false;
    }

    g_controllerBackupCaptureFailureLogged.store(
        false, std::memory_order_release);
    g_controllerBackupSnapshot.store(snapshot, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_controllerBackupSnapshotLock);
    return true;
}

bool RestoreControllerBackupSnapshot() {
    if (!g_loadSurfaceFromSurface) {
        return false;
    }

    AcquireSRWLockShared(&g_controllerBackupSnapshotLock);
    IDirect3DSurface9* const snapshot =
        g_controllerBackupSnapshot.load(std::memory_order_acquire);
    if (!snapshot) {
        ReleaseSRWLockShared(&g_controllerBackupSnapshotLock);
        return false;
    }

    IDirect3DSurface9* const destination =
        ResolveControllerBackupSurface();
    if (!destination) {
        ReleaseSRWLockShared(&g_controllerBackupSnapshotLock);
        return false;
    }

    const HRESULT result = g_loadSurfaceFromSurface(
        destination, nullptr, nullptr, snapshot, nullptr, nullptr,
        3u, 0u);
    ReleaseSRWLockShared(&g_controllerBackupSnapshotLock);
    if (FAILED(result)) {
        if (!g_controllerBackupRestoreFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            LogWarning("XIII controller-atlas snapshot restore failed after D3D9 Reset: result=0x%08lX",
                       static_cast<unsigned long>(result));
        }
        return false;
    }
    g_controllerBackupRestoreFailureLogged.store(
        false, std::memory_order_release);
    return true;
}

char __cdecl ReplaceKeyboardAtlasHook(void* atlas, char force) {
    const bool acquireLock = !g_nativeWrapperOwnsAtlasLock;
    if (acquireLock) AcquireSRWLockExclusive(&g_atlasMutationLock);
    CacheButtonAtlas(atlas);
    EnsureControllerBackupSnapshot();
    if (g_controllerBackupRefreshPending.load(
            std::memory_order_acquire) &&
        RestoreControllerBackupSnapshot()) {
        g_controllerBackupRefreshPending.store(
            false, std::memory_order_release);
    }
    ReplaceKeyboardAtlasFn original = g_replaceKeyboardAtlas;
    const char result = original ? original(atlas, force) : 0;
    if (acquireLock) ReleaseSRWLockExclusive(&g_atlasMutationLock);
    return result;
}

void __cdecl RestoreControllerAtlasHook(void* atlas) {
    const bool acquireLock = !g_nativeWrapperOwnsAtlasLock;
    if (acquireLock) AcquireSRWLockExclusive(&g_atlasMutationLock);
    CacheButtonAtlas(atlas);
    EnsureControllerBackupSnapshot();
    if (g_controllerBackupRefreshPending.load(
            std::memory_order_acquire) &&
        RestoreControllerBackupSnapshot()) {
        g_controllerBackupRefreshPending.store(
            false, std::memory_order_release);
    }
    RestoreControllerAtlasFn original = g_restoreControllerAtlas;
    if (original) original(atlas);
    if (acquireLock) ReleaseSRWLockExclusive(&g_atlasMutationLock);
}

int __cdecl PromptWrapperHook(char forceKeyboard) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13.prompt-native-wrapper-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13.prompt-native-wrapper-original");
    hook_profiler::Scope totalTiming(totalPoint);

    PromptWrapperFn original = g_originalPromptWrapper;
    if (!original) return 0;

    const NativeMode desired = DesiredModeNow();
    if (g_available.load(std::memory_order_acquire) &&
        desired != NativeMode::Unknown && desired == ReadActualMode() &&
        g_settingGetter) {
        return g_settingGetter(13);
    }

    // Lock the entire atlas transaction, including the native flag write
    // ApplyPendingMode shares this lock and rechecks the flag before copying
    AcquireSRWLockExclusive(&g_atlasMutationLock);
    g_nativeWrapperOwnsAtlasLock = true;
    int result = 0;
    {
        hook_profiler::Scope originalTiming(originalPoint);
        result = original(forceKeyboard);
    }
    g_nativeWrapperOwnsAtlasLock = false;
    ReleaseSRWLockExclusive(&g_atlasMutationLock);
    return result;
}

void ApplyPendingMode() {
    if (!g_available.load(std::memory_order_acquire)) return;

    // Preserve the pristine pad atlas in system memory before Reset loses it,
    // even when no prompt switch is pending
    if (AtlasReady()) {
        EnsureControllerBackupSnapshot();
    }

    const settings::PromptMode preference = settings::ActiveInput().promptMode;
    const input::InputSource activity = input::LatestActivity();
    const NativeMode desired = input::ResolvePromptMode(preference, activity);

    if (preference == settings::PromptMode::Auto) {
        const ULONGLONG now = GetTickCount64();
        if (desired != g_automaticPromptCandidate) {
            g_automaticPromptCandidate = desired;
            g_automaticPromptCandidateSince = now;
            return;
        }
        if (now - g_automaticPromptCandidateSince <
            input::kAutomaticPromptSwitchDebounceMs) {
            return;
        }
    } else {
        g_automaticPromptCandidate = NativeMode::Unknown;
        g_automaticPromptCandidateSince = 0u;
    }

    const bool recoveryPending =
        g_atlasRecoveryPending.load(std::memory_order_acquire);
    if (desired == NativeMode::Unknown ||
        (!recoveryPending && desired == ReadActualMode())) {
        return;
    }
    if (!AtlasReady()) return;

    void* atlas = CachedButtonAtlas();
    if (!atlas) return;

    // Never wait on a GUI worker from the frame thread. Retry next tick after
    // its atlas copy and prompt-flag write finish
    if (!TryAcquireSRWLockExclusive(&g_atlasMutationLock)) return;
    if (!recoveryPending && desired == ReadActualMode()) {
        ReleaseSRWLockExclusive(&g_atlasMutationLock);
        return;
    }

    if (recoveryPending && g_controllerBackupRefreshPending.load(
            std::memory_order_acquire)) {
        if (!RestoreControllerBackupSnapshot()) {
            ReleaseSRWLockExclusive(&g_atlasMutationLock);
            return;
        }
        g_controllerBackupRefreshPending.store(
            false, std::memory_order_release);
    }

    if (desired == NativeMode::Keyboard) {
        g_replaceKeyboardAtlas(atlas, 1);
        InterlockedExchange(g_nativePromptFlag, 1);
    } else {
        g_restoreControllerAtlas(atlas);
        InterlockedExchange(g_nativePromptFlag, 0);
    }
    ReleaseSRWLockExclusive(&g_atlasMutationLock);

    const ff13_prompt_layout_registry::RefreshResult refreshed =
        ff13_prompt_layout_registry::Refresh();
    g_atlasRecoveryPending.store(false, std::memory_order_release);

    Log("XIII button prompts switched: mode=%s source=%s preference=%u reset-recovery=%u registry-layout=%zu registry-direct=%zu",
        ModeName(desired), input::InputSourceName(activity),
        static_cast<unsigned>(preference),
        recoveryPending ? 1u : 0u,
        refreshed.layoutOwned, refreshed.direct);
}

int __fastcall InputTickHook(void* manager, void*, const std::int32_t* timing) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13.prompt-input-tick-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13.prompt-input-tick-original");
    hook_profiler::Scope totalTiming(totalPoint);
    InputTickFn original = g_originalInputTick;
    if (!original) return 0;
    int result = 0;
    {
        hook_profiler::Scope originalTiming(originalPoint);
        result = original(manager, timing);
    }
    ApplyPendingMode();
    return result;
}

std::array<std::byte, 13> RestoreControllerPrefix(std::uint32_t backupAddress) {
    std::array<std::byte, 13> prefix{
        std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
        std::byte{0x83}, std::byte{0xEC}, std::byte{0x20},
        std::byte{0x83}, std::byte{0x3D}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    std::memcpy(prefix.data() + 8, &backupAddress, sizeof(backupAddress));
    return prefix;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII ||
        !Supports(Capability::DynamicPrompts)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    const auto promptSignature = patch::Signature::Parse(
        "55 8B EC 81 EC 88 00 00 00 6A 0D E8 ?? ?? ?? ?? 83 C4 04 89 45 FC A1");
    const auto text = image ? image->Section(".text") : std::nullopt;
    if (!image || !promptSignature || !text) {
        LogWarning("XIII dynamic prompts disabled: executable image or signature unavailable");
        return TRUE;
    }

    const patch::SignatureMatch wrapperMatch = promptSignature->Find(text->bytes);
    if (wrapperMatch.count != 1 || !wrapperMatch.address ||
        std::memcmp(wrapperMatch.address, kPromptWrapperPrefix.data(),
                    kPromptWrapperPrefix.size()) != 0) {
        LogWarning("XIII dynamic prompts disabled: native wrapper matches=%zu",
            wrapperMatch.count);
        return TRUE;
    }

    std::byte* wrapper = const_cast<std::byte*>(wrapperMatch.address);
    std::byte* restoreController = wrapper + kRestoreControllerOffset;
    std::byte* replaceKeyboard = wrapper + kReplaceKeyboardOffset;
    const auto inputTickRange = image->At(kInputTickRva, kInputTickPrefix.size());
    const auto controllerBackupRange = image->At(
        kControllerAtlasBackupRva, sizeof(void*));
    const auto nativePromptFlagRange = image->At(
        kNativePromptFlagRva, sizeof(LONG));
    const auto settingGetterRange = image->At(
        kSettingGetterRva, 1u);
    const auto surfaceUnwrapRange = image->At(
        kSurfaceUnwrapRva, kSurfaceUnwrapPrefix.size());
    const auto controllerBackupAddress = image->Address32(
        kControllerAtlasBackupRva, sizeof(void*));
    if (!inputTickRange || !controllerBackupRange ||
        !nativePromptFlagRange || !controllerBackupAddress ||
        !settingGetterRange || !surfaceUnwrapRange) {
        LogWarning("XIII dynamic prompts disabled: native atlas RVA is outside the image or x86 address space");
        return TRUE;
    }
    std::byte* inputTick = const_cast<std::byte*>(inputTickRange->data());
    std::byte* controllerBackup = const_cast<std::byte*>(controllerBackupRange->data());
    std::byte* nativePromptFlag = const_cast<std::byte*>(nativePromptFlagRange->data());
    std::byte* settingGetter = const_cast<std::byte*>(
        settingGetterRange->data());
    std::byte* surfaceUnwrap = const_cast<std::byte*>(
        surfaceUnwrapRange->data());
    HMODULE const d3dx = GetModuleHandleW(L"d3dx9_43.dll");
    auto* const loadSurfaceFromSurface = d3dx
        ? reinterpret_cast<D3DXLoadSurfaceFromSurfaceFn>(
              GetProcAddress(d3dx, "D3DXLoadSurfaceFromSurface"))
        : nullptr;
    const auto restorePrefix = RestoreControllerPrefix(*controllerBackupAddress);

    const bool matches =
        image->Contains(restoreController, restorePrefix.size()) &&
        std::memcmp(restoreController, restorePrefix.data(), restorePrefix.size()) == 0 &&
        image->Contains(replaceKeyboard, kReplaceKeyboardPrefix.size()) &&
        std::memcmp(replaceKeyboard, kReplaceKeyboardPrefix.data(),
                    kReplaceKeyboardPrefix.size()) == 0 &&
        std::memcmp(inputTick, kInputTickPrefix.data(), kInputTickPrefix.size()) == 0 &&
        std::memcmp(surfaceUnwrap, kSurfaceUnwrapPrefix.data(),
                    kSurfaceUnwrapPrefix.size()) == 0 &&
        patch::memory::IsExecutable(settingGetter, 1u) &&
        loadSurfaceFromSurface && controllerBackup &&
        nativePromptFlag;
    if (!matches) {
        LogWarning("XIII dynamic prompts disabled: native atlas contract mismatch");
        return TRUE;
    }

    g_controllerAtlasBackup = reinterpret_cast<void**>(controllerBackup);
    g_nativePromptFlag = reinterpret_cast<volatile LONG*>(nativePromptFlag);
    g_settingGetter = reinterpret_cast<SettingGetterFn>(settingGetter);
    g_surfaceUnwrap = reinterpret_cast<SurfaceUnwrapFn>(surfaceUnwrap);
    g_loadSurfaceFromSurface = loadSurfaceFromSurface;

    if (!ff13_prompt_layout_registry::Install(*image)) {
        g_surfaceUnwrap = nullptr;
        g_loadSurfaceFromSurface = nullptr;
        LogWarning("XIII dynamic prompts disabled: prompt layout registry failed");
        return TRUE;
    }

    const bool wrapperInstalled = g_promptWrapperDetour.Install(
        "ff13-prompt-wrapper-ownership", wrapper,
        reinterpret_cast<void*>(&PromptWrapperHook),
        std::as_bytes(std::span{kPromptWrapperPrefix}),
        reinterpret_cast<void**>(&g_originalPromptWrapper));
    if (!wrapperInstalled) {
        g_originalPromptWrapper = nullptr;
        LogWarning("XIII native prompt-wrapper ownership unavailable: redundant saved-setting transactions may recur");
    }

    const bool replaceObserverInstalled =
        g_replaceKeyboardAtlasDetour.Install(
            "ff13-keyboard-atlas-observer", replaceKeyboard,
            reinterpret_cast<void*>(&ReplaceKeyboardAtlasHook),
            std::as_bytes(std::span{kReplaceKeyboardPrefix}),
            reinterpret_cast<void**>(&g_replaceKeyboardAtlas));
    const bool restoreObserverInstalled =
        replaceObserverInstalled &&
        g_restoreControllerAtlasDetour.Install(
            "ff13-controller-atlas-observer", restoreController,
            reinterpret_cast<void*>(&RestoreControllerAtlasHook),
            restorePrefix,
            reinterpret_cast<void**>(&g_restoreControllerAtlas));
    if (!replaceObserverInstalled || !restoreObserverInstalled) {
        g_restoreControllerAtlasDetour.RevertAndClear(
            g_restoreControllerAtlas);
        g_replaceKeyboardAtlasDetour.RevertAndClear(
            g_replaceKeyboardAtlas);
        g_promptWrapperDetour.RevertAndClear(g_originalPromptWrapper);
        ff13_prompt_layout_registry::Revert();
        g_settingGetter = nullptr;
        g_surfaceUnwrap = nullptr;
        g_loadSurfaceFromSurface = nullptr;
        LogWarning("XIII dynamic prompts disabled: native atlas observers failed");
        return TRUE;
    }

    if (!g_inputTickDetour.Install(
            "ff13-dynamic-prompts", inputTick,
            reinterpret_cast<void*>(&InputTickHook),
            std::as_bytes(std::span{kInputTickPrefix}),
            reinterpret_cast<void**>(&g_originalInputTick))) {
        const bool layoutRegistryReverted =
            ff13_prompt_layout_registry::Revert();
        const bool wrapperReverted = !wrapperInstalled ||
            g_promptWrapperDetour.RevertAndClear(g_originalPromptWrapper);
        const bool restoreObserverReverted =
            g_restoreControllerAtlasDetour.RevertAndClear(
                g_restoreControllerAtlas);
        const bool replaceObserverReverted =
            g_replaceKeyboardAtlasDetour.RevertAndClear(
                g_replaceKeyboardAtlas);
        if (layoutRegistryReverted && wrapperReverted &&
            restoreObserverReverted && replaceObserverReverted) {
            g_replaceKeyboardAtlas = nullptr;
            g_restoreControllerAtlas = nullptr;
            g_controllerAtlasBackup = nullptr;
            g_nativePromptFlag = nullptr;
            g_settingGetter = nullptr;
            g_surfaceUnwrap = nullptr;
            g_loadSurfaceFromSurface = nullptr;
        }
        LogWarning("XIII dynamic prompts disabled: input-update detour failed");
        return TRUE;
    }

    const bool lifecycleInstalled =
        ff13_prompt_layout_registry::InstallLifetimeObserver();
    if (!lifecycleInstalled) {
        LogWarning("XIII prompt RenderText lifecycle observer unavailable: stale capture entries reclaimed only on object reuse");
    }

    g_available.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void BeforeDeviceReset() {
    if (CurrentProfile().title != Title::FinalFantasyXIII ||
        !g_available.load(std::memory_order_acquire) ||
        !g_controllerAtlasBackup) {
        return;
    }

    // The game's wrapper crashes if Reset leaves it with a null surface
    // Restore the controller atlas from the system-memory copy instead
    g_controllerBackupRestoreFailureLogged.store(
        false, std::memory_order_release);
    const bool snapshotReady = EnsureControllerBackupSnapshot();
    g_atlasRecoveryPending.store(true, std::memory_order_release);
    g_controllerBackupRefreshPending.store(
        true, std::memory_order_release);
    Log("XIII controller-atlas backup marked stale before D3D9 Reset: restore=queued snapshot-ready=%u live-atlas=%p",
        snapshotReady ? 1u : 0u,
        g_cachedButtonAtlas.load(std::memory_order_acquire));
}

} // namespace novafix::game::ff13_prompt_mode
