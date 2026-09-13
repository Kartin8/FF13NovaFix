#include "game/titles/ff13/titan_route_correction.h"
#include "game/titles/ff13/titan_turn_angle.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <windows.h>

#include <intrin.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::ff13_titan_route_correction {
namespace {

constexpr std::uint32_t kFieldActorPostAttachCallRva = 0x1CD4C1u;
constexpr std::uint32_t kNativeFieldActorPostAttachRva = 0x1C1B60u;
constexpr std::uint32_t kActorRegistryGlobalRva = 0x22E80DCu;
constexpr std::uint32_t kResolveActorRva = 0x2A18B0u;
constexpr std::uint32_t kTitanMoveControlUpdateRva = 0x3D6ED0u;
constexpr std::uint32_t kTitanSignedTurnAngleRva = 0x3F1470u;
constexpr std::uint32_t kTitanYawDeadZoneGateRva = 0x3DFB41u;
constexpr std::uint32_t kTitanYawDeadZoneDecisionRva = 0x3DFB49u;
constexpr std::uint32_t kTitanYawAngleReturnRva = 0x3DF8EDu;
constexpr std::uint32_t kFrameManagerGlobalRva = 0x2401FACu;

constexpr std::size_t kFieldRecordActorHandleOffset = 0x60u;
constexpr std::size_t kActorInternalVtableOffset = 0x124u;
constexpr std::size_t kInternalCharacterSpecNameOffset = 0x338u;

constexpr std::int32_t kTitanHighFpsMaximumFrameMilliseconds = 30;
constexpr std::uint32_t kTitanHighFpsQualificationFrames = 8u;
constexpr std::uint32_t kTitanNativeFpsQualificationFrames = 8u;
constexpr ULONGLONG kTitanBindingCheckMilliseconds = 1000u;

constexpr std::array<std::uint8_t, 5> kFieldActorPostAttachCall{
    0xE8, 0x9A, 0x46, 0xFF, 0xFF,
};
constexpr std::array<std::uint8_t, 9> kMoveControlUpdatePrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x30,
};
constexpr std::array<std::uint8_t, 9> kSignedTurnAnglePrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x70,
};
constexpr std::array<std::uint8_t, 8> kYawDeadZoneGatePrefix{
    0xF3, 0x0F, 0x10, 0x4D, 0x98, 0x0F, 0x2F, 0xC8,
};

using FieldActorPostAttachFn = void*(__thiscall*)(void* record);
using ResolveActorFn = void*(__thiscall*)(void* registry,
                                         std::int32_t handle);
using ActorInternalFn = void*(__thiscall*)(void* actor);
using TitanMoveControlUpdateFn = unsigned int(__thiscall*)(
    void* control, void* actor, int, int);
using TitanSignedTurnAngleFn = float*(__cdecl*)(
    float* output, const void* desiredDirection,
    const void* currentDirection, const void* upDirection);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_titanMoveControlUpdateDetour;
patch::Detour32 g_titanSignedTurnAngleDetour;
patch::Detour32 g_titanYawDeadZoneGateDetour;

FieldActorPostAttachFn g_originalFieldActorPostAttach{};
ResolveActorFn g_resolveActor{};
TitanMoveControlUpdateFn g_originalTitanMoveControlUpdate{};
TitanSignedTurnAngleFn g_originalTitanSignedTurnAngle{};
void* g_originalTitanYawDeadZoneGate{};
void* g_titanYawDeadZoneDecision{};
void* g_titanYawAngleReturn{};

void** g_actorRegistryGlobal{};
void** g_frameManagerGlobal{};
std::atomic_int32_t g_titanActorHandle{-1};
std::atomic_uintptr_t g_titanActorPointer{};
std::atomic_uint64_t g_nextTitanBindingCheckMilliseconds{};
std::atomic_bool g_titanHighFps{};
std::uint32_t g_titanHighFpsQualificationCount{};
std::uint32_t g_titanNativeFpsQualificationCount{};
thread_local bool g_insideTitanMoveControlUpdate{};
thread_local bool g_applyTitanHighFpsCorrection{};
thread_local std::uint32_t g_titanYawDeadZoneBits{};

template <typename T>
bool ReadField(const void* object, std::size_t offset, T& value) {
    if (!object) return false;
    const auto* address = static_cast<const std::byte*>(object) + offset;
    if (!patch::memory::IsReadable(address, sizeof(T))) return false;
    std::memcpy(&value, address, sizeof(T));
    return true;
}

bool CopyFixedName(const void* object, std::size_t offset,
                   char (&output)[17]) {
    std::memset(output, 0, sizeof(output));
    if (!object) return false;
    const auto* address = static_cast<const std::byte*>(object) + offset;
    if (!patch::memory::IsReadable(address, 16u)) return false;
    std::memcpy(output, address, 16u);
    return true;
}

template <typename Function>
Function VirtualMethod(void* object, std::size_t methodOffset) {
    void* vtable{};
    if (!ReadField(object, 0u, vtable) || !vtable) return nullptr;

    void* method{};
    if (!ReadField(vtable, methodOffset, method) ||
        !patch::memory::IsExecutable(method, 1u)) {
        return nullptr;
    }
    return reinterpret_cast<Function>(method);
}

bool IsBoundTitan(const void* actor) {
    return actor && g_titanActorPointer.load(std::memory_order_acquire) ==
        reinterpret_cast<std::uintptr_t>(actor);
}

bool ReadFrameMilliseconds(std::int32_t& milliseconds) {
    void* manager{};
    return g_frameManagerGlobal &&
        ReadField(g_frameManagerGlobal, 0u, manager) && manager &&
        ReadField(manager, 0x4Cu, milliseconds) && milliseconds >= 0;
}

bool ReadTitanEffectiveDelta(void* actor, std::int32_t milliseconds,
                             float& delta) {
    if (!actor || milliseconds <= 0) return false;
    std::uint32_t actorFlags{};
    float timeScale{};
    std::memcpy(&actorFlags,
                static_cast<const std::byte*>(actor) + 0x6Cu,
                sizeof(actorFlags));
    std::memcpy(&timeScale,
                static_cast<const std::byte*>(actor) + 0x438u,
                sizeof(timeScale));
    if ((actorFlags & 0x20000u) != 0u || !std::isfinite(timeScale)) {
        return false;
    }
    delta = timeScale * static_cast<float>(milliseconds) / 1000.0f;
    return std::isfinite(delta) && delta > 0.0f;
}

bool UpdateTitanHighFpsGate() {
    std::int32_t milliseconds{};
    const bool fastFrame = ReadFrameMilliseconds(milliseconds) &&
        milliseconds > 0 &&
        milliseconds <= kTitanHighFpsMaximumFrameMilliseconds;
    if (fastFrame) {
        g_titanNativeFpsQualificationCount = 0u;
        if (g_titanHighFpsQualificationCount <
            kTitanHighFpsQualificationFrames) {
            ++g_titanHighFpsQualificationCount;
        }
        if (g_titanHighFpsQualificationCount >=
            kTitanHighFpsQualificationFrames) {
            g_titanHighFps.store(true, std::memory_order_release);
        }
    } else {
        g_titanHighFpsQualificationCount = 0u;
        if (g_titanNativeFpsQualificationCount <
            kTitanNativeFpsQualificationFrames) {
            ++g_titanNativeFpsQualificationCount;
        }
        if (g_titanNativeFpsQualificationCount >=
            kTitanNativeFpsQualificationFrames) {
            g_titanHighFps.store(false, std::memory_order_release);
        }
    }
    return g_titanHighFps.load(std::memory_order_acquire);
}

void ResetTitanBindingState() {
    g_titanHighFps.store(false, std::memory_order_release);
    g_titanHighFpsQualificationCount = 0u;
    g_titanNativeFpsQualificationCount = 0u;
}

bool HasTitanIdentity(void* actor) {
    char characterSpec[17]{};
    if (CopyFixedName(actor, kInternalCharacterSpecNameOffset,
                      characterSpec) &&
        std::strcmp(characterSpec, "f091") == 0) {
        return true;
    }

    const auto internalMethod =
        VirtualMethod<ActorInternalFn>(actor, kActorInternalVtableOffset);
    void* internal = internalMethod ? internalMethod(actor) : nullptr;
    return CopyFixedName(internal, kInternalCharacterSpecNameOffset,
                         characterSpec) &&
        std::strcmp(characterSpec, "f091") == 0;
}

void ValidateTitanBinding() {
    const std::uintptr_t bound =
        g_titanActorPointer.load(std::memory_order_acquire);
    if (bound == 0u) return;

    const ULONGLONG now = GetTickCount64();
    if (now < g_nextTitanBindingCheckMilliseconds.load(
                  std::memory_order_relaxed)) {
        return;
    }
    g_nextTitanBindingCheckMilliseconds.store(
        now + kTitanBindingCheckMilliseconds, std::memory_order_relaxed);

    void* registry{};
    if (!ReadField(g_actorRegistryGlobal, 0u, registry) || !registry ||
        !g_resolveActor) {
        return;
    }
    const std::int32_t handle =
        g_titanActorHandle.load(std::memory_order_acquire);
    void* actor = handle != -1 ? g_resolveActor(registry, handle) : nullptr;
    if (actor == reinterpret_cast<void*>(bound) && HasTitanIdentity(actor)) {
        return;
    }

    std::uintptr_t expected = bound;
    if (g_titanActorPointer.compare_exchange_strong(
            expected, 0u, std::memory_order_acq_rel)) {
        g_titanActorHandle.store(-1, std::memory_order_release);
        ResetTitanBindingState();
    }
}

void* __fastcall FieldActorPostAttachHook(void* record, void*) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13.titan-field-actor-attach-total");
    void* result = g_originalFieldActorPostAttach
        ? g_originalFieldActorPostAttach(record) : record;

    std::int32_t handle{-1};
    void* registry{};
    if (!ReadField(record, kFieldRecordActorHandleOffset, handle) ||
        !ReadField(g_actorRegistryGlobal, 0u, registry) || !registry ||
        !g_resolveActor) {
        return result;
    }

    void* actor = g_resolveActor(registry, handle);
    if (!HasTitanIdentity(actor)) return result;

    ResetTitanBindingState();
    g_titanActorHandle.store(handle, std::memory_order_release);
    g_titanActorPointer.store(reinterpret_cast<std::uintptr_t>(actor),
                              std::memory_order_release);
    g_nextTitanBindingCheckMilliseconds.store(0u,
                                               std::memory_order_relaxed);
    return result;
}

float* __cdecl TitanSignedTurnAngleHook(
    float* output, const void* desiredDirection,
    const void* currentDirection, const void* upDirection) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13.titan-turn-angle-total");
    float* result = g_originalTitanSignedTurnAngle
        ? g_originalTitanSignedTurnAngle(output, desiredDirection,
                                         currentDirection, upDirection)
        : output;
    if (!g_insideTitanMoveControlUpdate ||
        _ReturnAddress() != g_titanYawAngleReturn || !result ||
        !std::isfinite(*result)) {
        return result;
    }

    float stableAngle{};
    std::array<float, 3> desired{}, current{}, up{};
    bool stableValid{};
    if (desiredDirection && currentDirection && upDirection) {
        std::memcpy(desired.data(), desiredDirection, sizeof(desired));
        std::memcpy(current.data(), currentDirection, sizeof(current));
        std::memcpy(up.data(), upDirection, sizeof(up));
        stableValid = TryComputeTurnAngle(
            desired, current, up, stableAngle);
    }
    if (g_applyTitanHighFpsCorrection && stableValid) {
        *result = stableAngle;
    }
    return result;
}

bool __cdecl ShouldApplyTitanHighFpsCorrection() {
    return g_applyTitanHighFpsCorrection;
}

std::uint32_t __cdecl TitanYawDeadZoneBits() {
    return g_titanYawDeadZoneBits;
}

#if defined(_M_IX86) || defined(__i386__)
void __declspec(naked) TitanYawDeadZoneGateHook() {
    __asm {
        sub esp, 20h
        movups xmmword ptr [esp], xmm0
        movups xmmword ptr [esp + 10h], xmm1
        pushfd
        pushad
        call ShouldApplyTitanHighFpsCorrection
        test al, al
        jnz scaled_dead_zone
        popad
        popfd
        movups xmm0, xmmword ptr [esp]
        movups xmm1, xmmword ptr [esp + 10h]
        add esp, 20h
        jmp dword ptr [g_originalTitanYawDeadZoneGate]
    scaled_dead_zone:
        call TitanYawDeadZoneBits
        mov dword ptr [esp + 34h], eax
        popad
        popfd
        movups xmm0, xmmword ptr [esp]
        movups xmm1, xmmword ptr [esp + 10h]
        add esp, 20h
        comiss xmm1, xmm0
        jmp dword ptr [g_titanYawDeadZoneDecision]
    }
}
#else
void TitanYawDeadZoneGateHook() {}
#endif

unsigned int __fastcall TitanMoveControlUpdateHook(
    void* control, void*, void* actor, int a3, int a4) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13.titan-move-control-total");
    const bool titan = IsBoundTitan(actor);
    const bool previousTitanScope = g_insideTitanMoveControlUpdate;
    const bool previousCorrectionScope = g_applyTitanHighFpsCorrection;
    const std::uint32_t previousDeadZoneBits = g_titanYawDeadZoneBits;
    bool behaviorScope{};
    std::uint32_t packedState{};
    std::uint32_t options{};
    const bool hasPackedState = titan &&
        ReadField(control, 0x214u, packedState);
    const bool hasOptions = titan && ReadField(control, 0x180u, options);
    const bool highFps = titan && UpdateTitanHighFpsGate();
    behaviorScope = highFps && hasPackedState && hasOptions &&
        (packedState & 0xFFu) == 3u &&
        ((packedState >> 8u) & 0xFFu) == 0x0Du &&
        (options & 2u) == 0u;
    if (behaviorScope) {
        std::int32_t milliseconds{};
        float delta{};
        if (!ReadFrameMilliseconds(milliseconds) ||
            !ReadTitanEffectiveDelta(actor, milliseconds, delta)) {
            behaviorScope = false;
        } else {
            const float threshold = ComputeYawDeadZone(
                0.001f, delta, true);
            std::memcpy(&g_titanYawDeadZoneBits, &threshold,
                        sizeof(threshold));
        }
    }

    g_insideTitanMoveControlUpdate = previousTitanScope || titan;
    g_applyTitanHighFpsCorrection =
        previousCorrectionScope || behaviorScope;
    const unsigned int result = g_originalTitanMoveControlUpdate
        ? g_originalTitanMoveControlUpdate(control, actor, a3, a4) : 0u;
    g_titanYawDeadZoneBits = previousDeadZoneBits;
    g_applyTitanHighFpsCorrection = previousCorrectionScope;
    g_insideTitanMoveControlUpdate = previousTitanScope;
    return result;
}

bool InstallTitanIdentityHook(const ImageView& image) {
    const auto callRange = image.At(kFieldActorPostAttachCallRva,
                                    kFieldActorPostAttachCall.size());
    const auto nativePostAttach = image.At(kNativeFieldActorPostAttachRva, 1u);
    const auto registryGlobal = image.At(kActorRegistryGlobalRva,
                                         sizeof(void*));
    const auto resolveActor = image.At(kResolveActorRva, 1u);
    if (!callRange || !nativePostAttach || !registryGlobal || !resolveActor ||
        std::memcmp(callRange->data(), kFieldActorPostAttachCall.data(),
                    kFieldActorPostAttachCall.size()) != 0) {
        return false;
    }

    std::byte* call = const_cast<std::byte*>(callRange->data());
    const auto replacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(call),
        reinterpret_cast<std::uintptr_t>(&FieldActorPostAttachHook));
    if (!replacement) return false;

    patch::Transaction transaction(
        "ff13-titan-identity",
        std::span<std::byte>(call, kFieldActorPostAttachCall.size()),
        patch::registry::Kind::Detour);
    if (!transaction.Add(
            0u, std::as_bytes(std::span{kFieldActorPostAttachCall}),
            *replacement)) {
        return false;
    }

    g_originalFieldActorPostAttach = reinterpret_cast<FieldActorPostAttachFn>(
        const_cast<std::byte*>(nativePostAttach->data()));
    g_actorRegistryGlobal = reinterpret_cast<void**>(
        const_cast<std::byte*>(registryGlobal->data()));
    g_resolveActor = reinterpret_cast<ResolveActorFn>(
        const_cast<std::byte*>(resolveActor->data()));

    if (transaction.Apply() != patch::ApplyStatus::Applied) {
        g_originalFieldActorPostAttach = nullptr;
        g_actorRegistryGlobal = nullptr;
        g_resolveActor = nullptr;
        return false;
    }
    return true;
}

void RevertTitanSteeringCorrection() {
    const bool yawGateReverted =
        g_titanYawDeadZoneGateDetour.RevertAndClear(
            g_originalTitanYawDeadZoneGate);
    if (yawGateReverted) {
        g_titanYawDeadZoneDecision = nullptr;
        g_titanYawAngleReturn = nullptr;
    }
    g_titanSignedTurnAngleDetour.RevertAndClear(
        g_originalTitanSignedTurnAngle);
    g_titanMoveControlUpdateDetour.RevertAndClear(
        g_originalTitanMoveControlUpdate);
}

bool InstallTitanSteeringCorrection(const ImageView& image) {
    const auto moveControl = image.At(kTitanMoveControlUpdateRva,
                                      kMoveControlUpdatePrefix.size());
    const auto signedTurnAngle = image.At(kTitanSignedTurnAngleRva,
                                          kSignedTurnAnglePrefix.size());
    const auto yawDeadZoneGate = image.At(kTitanYawDeadZoneGateRva,
                                         kYawDeadZoneGatePrefix.size());
    const auto yawDeadZoneDecision = image.At(kTitanYawDeadZoneDecisionRva, 1u);
    const auto yawAngleReturn = image.At(kTitanYawAngleReturnRva, 1u);
    const auto frameManagerGlobal = image.At(kFrameManagerGlobalRva,
                                             sizeof(void*));
    if (!moveControl || !signedTurnAngle || !yawDeadZoneGate ||
        !yawDeadZoneDecision || !yawAngleReturn ||
        !frameManagerGlobal) {
        return false;
    }

    g_titanYawDeadZoneDecision = const_cast<std::byte*>(
        yawDeadZoneDecision->data());
    g_titanYawAngleReturn = const_cast<std::byte*>(yawAngleReturn->data());
    g_frameManagerGlobal = reinterpret_cast<void**>(const_cast<std::byte*>(
        frameManagerGlobal->data()));

    bool installed = g_titanMoveControlUpdateDetour.Install(
        "ff13-titan-move-control",
        const_cast<std::byte*>(moveControl->data()),
        reinterpret_cast<void*>(&TitanMoveControlUpdateHook),
        std::as_bytes(std::span{kMoveControlUpdatePrefix}),
        reinterpret_cast<void**>(&g_originalTitanMoveControlUpdate));
    installed = installed && g_titanSignedTurnAngleDetour.Install(
        "ff13-titan-turn-angle",
        const_cast<std::byte*>(signedTurnAngle->data()),
        reinterpret_cast<void*>(&TitanSignedTurnAngleHook),
        std::as_bytes(std::span{kSignedTurnAnglePrefix}),
        reinterpret_cast<void**>(&g_originalTitanSignedTurnAngle));
    installed = installed && g_titanYawDeadZoneGateDetour.Install(
        "ff13-titan-yaw-dead-zone",
        const_cast<std::byte*>(yawDeadZoneGate->data()),
        reinterpret_cast<void*>(&TitanYawDeadZoneGateHook),
        std::as_bytes(std::span{kYawDeadZoneGatePrefix}),
        &g_originalTitanYawDeadZoneGate);
    if (!installed) {
        RevertTitanSteeringCorrection();
        g_frameManagerGlobal = nullptr;
    }
    return installed;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII ||
        !Supports(Capability::TitanRouteCorrection)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII Titan route correction disabled: main image unavailable");
        return TRUE;
    }

    const bool identityInstalled = InstallTitanIdentityHook(*image);
    const bool steeringInstalled = identityInstalled &&
        InstallTitanSteeringCorrection(*image);
    if (!identityInstalled || !steeringInstalled) {
        LogWarning("XIII Titan high-FPS route correction disabled: "
                   "identity=%d steering=%d",
                   identityInstalled, steeringInstalled);
    }
    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void Pump() {
    ValidateTitanBinding();
}

} // namespace novafix::game::ff13_titan_route_correction
