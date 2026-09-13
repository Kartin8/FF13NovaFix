#include "game/titles/ff13/animation/motion_blend_correction.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/titles/ff13/animation/motion_blend_policy.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::ff13_motion_blend_correction {
namespace {

constexpr std::uint32_t kLayerActivationRva = 0xB928D0u;
constexpr std::uint32_t kEffectiveJointWeightRva = 0xB94040u;
constexpr std::size_t kActivationSlots = 1024u;
constexpr std::size_t kMotionResourceOffset = 0x78u;
constexpr std::size_t kMotionNameLength = 64u;

constexpr std::array<std::uint8_t, 9> kLayerActivationPrefix{
    0x83, 0xEC, 0x0C, 0x55, 0x8B, 0xE9, 0x8B, 0x4C, 0x24};
constexpr std::array<std::uint8_t, 9> kEffectiveJointWeightPrefix{
    0x8B, 0x51, 0x38, 0x85, 0xD2, 0x74, 0x10, 0x8B, 0x41};

using LayerActivationFn = void(__thiscall*)(
    void* activator, void* layer, void* channel, std::uint32_t priority);
using EffectiveJointWeightFn = float(__thiscall*)(
    void* layer, std::uint32_t jointIndex);
using MotionResourceNameFn = const char*(__thiscall*)(void* resource);

struct Activation {
    std::atomic_uintptr_t layer{};
    std::atomic_uint64_t milliseconds{};
    std::atomic_bool locomotion{};
};

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_layerActivationDetour;
patch::Detour32 g_effectiveJointWeightDetour;
LayerActivationFn g_originalLayerActivation{};
EffectiveJointWeightFn g_originalEffectiveJointWeight{};
std::array<Activation, kActivationSlots> g_activations{};

std::size_t ActivationSlot(const void* layer) {
    return (reinterpret_cast<std::uintptr_t>(layer) >> 4u) %
        g_activations.size();
}

void RecordActivation(void* layer, bool locomotion) {
    if (!layer) return;
    Activation& activation = g_activations[ActivationSlot(layer)];
    activation.locomotion.store(locomotion, std::memory_order_relaxed);
    activation.milliseconds.store(GetTickCount64(),
                                  std::memory_order_relaxed);
    activation.layer.store(reinterpret_cast<std::uintptr_t>(layer),
                           std::memory_order_release);
}

bool CopyNativeMotionName(const void* layer,
                          char (&name)[kMotionNameLength + 1u]);

void __fastcall LayerActivationHook(void* activator, void*, void* layer,
                                    void* channel,
                                    std::uint32_t priority) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13.motion-layer-activation-total");
    char name[kMotionNameLength + 1u]{};
    const bool hasName = CopyNativeMotionName(layer, name);
    const bool locomotion = hasName &&
        ff13_motion_blend_policy::IsFieldLocomotionResource(name);
    RecordActivation(layer, locomotion);
    if (g_originalLayerActivation) {
        g_originalLayerActivation(activator, layer, channel, priority);
    }
}

float __fastcall EffectiveJointWeightHook(void* layer, void*,
                                          std::uint32_t jointIndex) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.ff13.effective-joint-weight-total", 256u);
    EffectiveJointWeightFn original = g_originalEffectiveJointWeight;
    if (!original) return 1.0f;

    const float nativeWeight = original(layer, jointIndex);
    if (!layer) return nativeWeight;

    const Activation& activation = g_activations[ActivationSlot(layer)];
    if (activation.layer.load(std::memory_order_acquire) !=
        reinterpret_cast<std::uintptr_t>(layer)) {
        return nativeWeight;
    }
    if (!activation.locomotion.load(std::memory_order_relaxed)) {
        return nativeWeight;
    }

    const ULONGLONG started =
        activation.milliseconds.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    const bool hasActivation = started != 0u && now >= started;
    const std::uint64_t elapsed = hasActivation ? now - started : 0u;
    return ff13_motion_blend_policy::ScaleActivatedWeight(
        nativeWeight, elapsed, hasActivation);
}

template <typename T>
bool ReadField(const void* object, std::size_t offset, T& value) {
    if (!object) return false;
    const auto* address = static_cast<const std::byte*>(object) + offset;
    if (!patch::memory::IsReadable(address, sizeof(T))) return false;
    std::memcpy(&value, address, sizeof(T));
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

bool CopyNativeMotionName(const void* layer,
                          char (&name)[kMotionNameLength + 1u]) {
    std::memset(name, 0, sizeof(name));
    void* resource{};
    if (!layer || !ReadField(layer, kMotionResourceOffset, resource) ||
        !resource) {
        return false;
    }

    const MotionResourceNameFn nativeName =
        VirtualMethod<MotionResourceNameFn>(resource, 4u);
    if (!nativeName) return false;
    const char* source = nativeName(resource);
    if (!source) return false;

    for (std::size_t index = 0u; index < kMotionNameLength; ++index) {
        if (!patch::memory::IsReadable(source + index, 1u)) return false;
        name[index] = source[index];
        if (name[index] == '\0') return index != 0u;
    }
    name[kMotionNameLength] = '\0';
    return true;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII ||
        !Supports(Capability::MotionBlendCorrection)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII motion blend correction disabled: main image unavailable");
        return TRUE;
    }

    const auto activation = image->At(
        kLayerActivationRva, kLayerActivationPrefix.size());
    const auto effectiveWeight = image->At(
        kEffectiveJointWeightRva, kEffectiveJointWeightPrefix.size());
    const bool activationMatches = activation &&
        std::memcmp(activation->data(), kLayerActivationPrefix.data(),
                    kLayerActivationPrefix.size()) == 0;
    const bool effectiveWeightMatches = effectiveWeight &&
        std::memcmp(effectiveWeight->data(),
                    kEffectiveJointWeightPrefix.data(),
                    kEffectiveJointWeightPrefix.size()) == 0;
    if (!activationMatches || !effectiveWeightMatches) {
        LogWarning("XIII motion blend correction disabled: contract mismatch "
            "activation=%d weight=%d", activationMatches,
            effectiveWeightMatches);
        return TRUE;
    }

    const bool activationInstalled = g_layerActivationDetour.Install(
        "ff13-motion-blend-activation",
        const_cast<std::byte*>(activation->data()),
        reinterpret_cast<void*>(&LayerActivationHook),
        std::as_bytes(std::span{kLayerActivationPrefix}),
        reinterpret_cast<void**>(&g_originalLayerActivation));
    const bool weightInstalled = activationInstalled &&
        g_effectiveJointWeightDetour.Install(
            "ff13-motion-blend-weight",
            const_cast<std::byte*>(effectiveWeight->data()),
            reinterpret_cast<void*>(&EffectiveJointWeightHook),
            std::as_bytes(std::span{kEffectiveJointWeightPrefix}),
            reinterpret_cast<void**>(&g_originalEffectiveJointWeight));
    if (!weightInstalled) {
        if (activationInstalled) {
            g_layerActivationDetour.RevertAndClear(
                g_originalLayerActivation);
        }
        g_originalEffectiveJointWeight = nullptr;
        LogWarning("XIII motion blend correction disabled: hook installation failed");
        return TRUE;
    }
    return TRUE;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

} // namespace novafix::game::ff13_motion_blend_correction
