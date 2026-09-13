#include "game/titles/ff13_2/ui/qte/qte_prompt_lifecycle.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "game/titles/ff13_2/ui/qte/qte_visual_bundles.h"
#include "patch/detour32.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::xiii2_qte_prompts {
namespace {

constexpr std::uint32_t kOwnerConstructorRva = 0x003B22B0u;
constexpr std::uint32_t kOwnerDestructorRva = 0x003B23F0u;
constexpr std::uint32_t kTextureBindingRva = 0x003AC750u;
constexpr std::size_t kOwnerPromptModeOffset = 0xF8u;
constexpr char kCinemaAtlasName[] = "cinema_parts02";

constexpr std::array<std::uint8_t, 9> kOwnerConstructorPrefix{
    0x53, 0x56, 0x8B, 0xF1, 0xE8, 0x37, 0x0C, 0x0E, 0x00};
constexpr std::array<std::uint8_t, 12> kOwnerDestructorPrefix{
    0x53, 0x56, 0x57, 0x8B, 0xF9, 0x83,
    0xBF, 0xEC, 0x00, 0x00, 0x00, 0xFF};
constexpr std::array<std::uint8_t, 7> kTextureBindingPrefix{
    0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1};

using NativeMode = input::NativePromptMode;
using OwnerConstructorFn = void* (__thiscall*)(void* owner);
using OwnerDestructorFn = void* (__thiscall*)(void* owner);
using TextureBindingFn = int (__thiscall*)(void* binding,
                                           const void* descriptor);

struct EpochSnapshot {
    NativeMode mode{NativeMode::Unknown};
    std::uint64_t generation{};

    bool active() const {
        return generation != 0u && mode != NativeMode::Unknown;
    }
};

patch::Detour32 g_ownerConstructorDetour;
patch::Detour32 g_ownerDestructorDetour;
patch::Detour32 g_textureBindingDetour;
OwnerConstructorFn g_originalOwnerConstructor{};
OwnerDestructorFn g_originalOwnerDestructor{};
TextureBindingFn g_originalTextureBinding{};
NativeFunctions g_nativeFunctions{};

SRWLOCK g_epochLock = SRWLOCK_INIT;
std::atomic_uint32_t g_activeOwners{0u};
std::atomic<int> g_lockedMode{static_cast<int>(NativeMode::Unknown)};
std::atomic_ullong g_epochGeneration{0u};
std::atomic_ullong g_appliedAtlasGeneration{0u};
std::atomic_ullong g_pendingAtlasGeneration{0u};
std::atomic_flag g_applyingAtlas = ATOMIC_FLAG_INIT;
std::atomic_bool g_destructorUnderflowLogged{false};
thread_local bool g_insideAtlasApplication{};

NativeMode SelectEpochMode() {
    NativeMode mode = NativeMode::Unknown;
    if (g_nativeFunctions.readRequestedMode) {
        mode = g_nativeFunctions.readRequestedMode();
    }
    if (mode == NativeMode::Unknown && g_nativeFunctions.readAppliedMode) {
        mode = g_nativeFunctions.readAppliedMode();
    }
    return mode == NativeMode::Unknown ? NativeMode::Controller : mode;
}

EpochSnapshot ReadEpoch() {
    if (g_activeOwners.load(std::memory_order_acquire) == 0u) return {};

    EpochSnapshot snapshot{
        static_cast<NativeMode>(g_lockedMode.load(std::memory_order_acquire)),
        g_epochGeneration.load(std::memory_order_acquire),
    };
    if (g_activeOwners.load(std::memory_order_acquire) == 0u) return {};
    return snapshot;
}

EpochSnapshot AcquireEpoch() {
    AcquireSRWLockExclusive(&g_epochLock);

    const std::uint32_t previousOwners =
        g_activeOwners.load(std::memory_order_relaxed);
    if (previousOwners == 0u) {
        const NativeMode mode = SelectEpochMode();

        const std::uint64_t generation =
            g_epochGeneration.load(std::memory_order_relaxed) + 1u;
        g_lockedMode.store(static_cast<int>(mode), std::memory_order_relaxed);
        g_epochGeneration.store(generation, std::memory_order_release);
    }

    g_activeOwners.store(previousOwners + 1u, std::memory_order_release);
    const EpochSnapshot snapshot{
        static_cast<NativeMode>(g_lockedMode.load(std::memory_order_relaxed)),
        g_epochGeneration.load(std::memory_order_relaxed),
    };

    ReleaseSRWLockExclusive(&g_epochLock);
    return snapshot;
}

void ReleaseEpoch() {
    AcquireSRWLockExclusive(&g_epochLock);

    const std::uint32_t previousOwners =
        g_activeOwners.load(std::memory_order_relaxed);
    if (previousOwners == 0u) {
        ReleaseSRWLockExclusive(&g_epochLock);
        if (!g_destructorUnderflowLogged.exchange(
                true, std::memory_order_acq_rel)) {
            LogWarning("XIII-2 QTE owner destructor observed without an active visual epoch");
        }
        return;
    }

    const std::uint32_t remainingOwners = previousOwners - 1u;
    g_activeOwners.store(remainingOwners, std::memory_order_release);
    if (remainingOwners == 0u) {
        g_lockedMode.store(static_cast<int>(NativeMode::Unknown),
                           std::memory_order_release);
        g_pendingAtlasGeneration.store(0u, std::memory_order_release);
    }

    ReleaseSRWLockExclusive(&g_epochLock);
}

bool ApplyEpochMode(const EpochSnapshot& epoch, bool forceTransaction,
                    bool markAtlasGeneration) {
    if (!epoch.active() || g_insideAtlasApplication ||
        (markAtlasGeneration &&
         g_appliedAtlasGeneration.load(std::memory_order_acquire) ==
             epoch.generation)) {
        return false;
    }
    if (!forceTransaction && g_nativeFunctions.readAppliedMode() == epoch.mode) {
        return false;
    }
    if (g_applyingAtlas.test_and_set(std::memory_order_acquire)) {
        return false;
    }

    const EpochSnapshot current = ReadEpoch();
    if (!current.active() || current.generation != epoch.generation ||
        current.mode != epoch.mode) {
        g_applyingAtlas.clear(std::memory_order_release);
        return false;
    }

    g_insideAtlasApplication = true;
    g_nativeFunctions.applyMode(epoch.mode);
    g_insideAtlasApplication = false;
    if (markAtlasGeneration) {
        g_appliedAtlasGeneration.store(epoch.generation,
                                       std::memory_order_release);
    }
    g_applyingAtlas.clear(std::memory_order_release);
    return true;
}

void* __fastcall OwnerConstructorHook(void* owner, void*) {
    OwnerConstructorFn original = g_originalOwnerConstructor;
    if (!original) return owner;

    const EpochSnapshot epoch = AcquireEpoch();
    // The native manager builds all mode-specific children immediately after
    // this constructor returns. Synchronize the global atlas and native flag
    // first, then override the PC-only snapshot produced by the constructor
    // Do not mark the cinema atlas generation here: if cinema_parts02 is
    // materialized later, its binding hook must repeat the same frozen mode
    ApplyEpochMode(epoch, false, false);
    void* result = original(owner);
    if (!result) {
        ReleaseEpoch();
        return nullptr;
    }

    const std::uint32_t keyboardFamily =
        epoch.mode == NativeMode::Keyboard ? 1u : 0u;
    std::memcpy(static_cast<std::byte*>(result) + kOwnerPromptModeOffset,
                &keyboardFamily, sizeof(keyboardFamily));
    xiii2_qte_visuals::RegisterOwner(result, epoch.mode);
    return result;
}

void* __fastcall OwnerDestructorHook(void* owner, void*) {
    OwnerDestructorFn original = g_originalOwnerDestructor;
    if (!original) return owner;

    xiii2_qte_visuals::UnregisterOwner(owner);
    void* result = original(owner);
    ReleaseEpoch();
    return result;
}

int __fastcall TextureBindingHook(void* binding, void*,
                                  const void* descriptor) {
    TextureBindingFn original = g_originalTextureBinding;
    if (!original) return 0;

    const EpochSnapshot epoch = ReadEpoch();
    if (!epoch.active()) {
        return original(binding, descriptor);
    }

    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.qte-texture-binding-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.ff13-2.qte-texture-binding-original");
    thread_local std::uint32_t totalSampleCounter{};
    hook_profiler::Scope totalTiming(
        totalPoint, (totalSampleCounter++ & 63u) == 0u, 64u);

    int result = 0;
    {
        thread_local std::uint32_t originalSampleCounter{};
        hook_profiler::Scope originalTiming(
            originalPoint,
            (originalSampleCounter++ & 63u) == 0u, 64u);
        result = original(binding, descriptor);
    }

    if (!descriptor || g_insideAtlasApplication) {
        return result;
    }
    if (std::strcmp(static_cast<const char*>(descriptor),
                    kCinemaAtlasName) != 0) {
        return result;
    }

    // Replacement from inside the binding stack can be overwritten by later
    // members of the same texture-load batch. Coalesce matching bindings and
    // apply once from the next game-thread input update
    g_appliedAtlasGeneration.store(0u, std::memory_order_release);
    g_pendingAtlasGeneration.store(epoch.generation,
                                   std::memory_order_release);
    return result;
}

bool InstallDetours(const ImageView& image) {
    const auto constructorRange = image.At(
        kOwnerConstructorRva, kOwnerConstructorPrefix.size());
    const auto destructorRange = image.At(
        kOwnerDestructorRva, kOwnerDestructorPrefix.size());
    const auto textureBindingRange = image.At(
        kTextureBindingRva, kTextureBindingPrefix.size());
    if (!constructorRange || !destructorRange || !textureBindingRange) {
        return false;
    }

    if (!xiii2_qte_visuals::Install(image)) return false;

    const bool destructorInstalled = g_ownerDestructorDetour.Install(
        "ff13-2-qte-owner-destructor",
        const_cast<std::byte*>(destructorRange->data()),
        reinterpret_cast<void*>(&OwnerDestructorHook),
        std::as_bytes(std::span{kOwnerDestructorPrefix}),
        reinterpret_cast<void**>(&g_originalOwnerDestructor));
    if (!destructorInstalled) {
        xiii2_qte_visuals::Revert();
        return false;
    }

    const bool constructorInstalled = g_ownerConstructorDetour.Install(
        "ff13-2-qte-owner-constructor",
        const_cast<std::byte*>(constructorRange->data()),
        reinterpret_cast<void*>(&OwnerConstructorHook),
        std::as_bytes(std::span{kOwnerConstructorPrefix}),
        reinterpret_cast<void**>(&g_originalOwnerConstructor));
    if (!constructorInstalled) {
        g_ownerDestructorDetour.RevertAndClear(g_originalOwnerDestructor);
        xiii2_qte_visuals::Revert();
        return false;
    }

    const bool bindingInstalled = g_textureBindingDetour.Install(
        "ff13-2-qte-atlas-late-load",
        const_cast<std::byte*>(textureBindingRange->data()),
        reinterpret_cast<void*>(&TextureBindingHook),
        std::as_bytes(std::span{kTextureBindingPrefix}),
        reinterpret_cast<void**>(&g_originalTextureBinding));
    if (!bindingInstalled) {
        LogWarning("XIII-2 QTE atlas late-load gate unavailable: owner mode locking remains active");
    }
    return true;
}

} // namespace

bool HasExpectedContract(const ImageView& image) {
    const auto constructor = image.At(
        kOwnerConstructorRva, kOwnerConstructorPrefix.size());
    const auto destructor = image.At(
        kOwnerDestructorRva, kOwnerDestructorPrefix.size());
    const auto textureBinding = image.At(
        kTextureBindingRva, kTextureBindingPrefix.size());
    return constructor && destructor && textureBinding &&
        xiii2_qte_visuals::HasExpectedContract(image) &&
        std::memcmp(constructor->data(), kOwnerConstructorPrefix.data(),
                    kOwnerConstructorPrefix.size()) == 0 &&
        std::memcmp(destructor->data(), kOwnerDestructorPrefix.data(),
                    kOwnerDestructorPrefix.size()) == 0 &&
        std::memcmp(textureBinding->data(), kTextureBindingPrefix.data(),
                    kTextureBindingPrefix.size()) == 0;
}

bool InstallLifecycle(const ImageView& image,
                      const NativeFunctions& functions) {
    if (!functions.readRequestedMode || !functions.readAppliedMode ||
        !functions.applyMode || !HasExpectedContract(image)) {
        return false;
    }

    g_nativeFunctions = functions;
    if (InstallDetours(image)) return true;

    g_nativeFunctions = {};
    return false;
}

input::NativePromptMode ResolveMode(input::NativePromptMode requested) {
    const EpochSnapshot epoch = ReadEpoch();
    return epoch.active() ? epoch.mode : requested;
}

input::NativePromptMode ApplyRequestedMode(
    input::NativePromptMode requested) {
    const EpochSnapshot epoch = ReadEpoch();
    if (!epoch.active() || requested == NativeMode::Unknown) return requested;
    if (requested == epoch.mode) return epoch.mode;

    const auto result = xiii2_qte_visuals::SwitchAll(requested);
    if (result != xiii2_qte_visuals::SwitchResult::Switched &&
        result != xiii2_qte_visuals::SwitchResult::Unchanged) {
        return epoch.mode;
    }

    AcquireSRWLockExclusive(&g_epochLock);
    const bool sameEpoch =
        g_activeOwners.load(std::memory_order_relaxed) != 0u &&
        g_epochGeneration.load(std::memory_order_relaxed) == epoch.generation;
    if (sameEpoch) {
        g_lockedMode.store(static_cast<int>(requested),
                           std::memory_order_release);
        g_appliedAtlasGeneration.store(0u, std::memory_order_release);
        g_pendingAtlasGeneration.store(0u, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_epochLock);

    return requested;
}

void FlushPendingAtlas() {
    const EpochSnapshot epoch = ReadEpoch();
    if (!epoch.active() ||
        g_pendingAtlasGeneration.load(std::memory_order_acquire) !=
            epoch.generation) {
        return;
    }
    if (!ApplyEpochMode(epoch, true, true)) return;

    std::uint64_t expected = epoch.generation;
    g_pendingAtlasGeneration.compare_exchange_strong(
        expected, 0u, std::memory_order_acq_rel);
}

} // namespace novafix::game::xiii2_qte_prompts
