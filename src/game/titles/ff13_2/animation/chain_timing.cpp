#include "game/titles/ff13_2/animation/chain_timing.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/shared/timing/frame_pacing.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_chain_timing {
namespace {

// The native wrapper rounds a sub-1/30 delta up to a full 1/30 update
// It is safe on Xbox at 30 Hz but doubles chain speed at 60 Hz on PC
constexpr std::uint32_t kChainFixedStepRva = 0x0078B370u;
constexpr std::uint32_t kChainStepConstantRva = 0x0096192Cu;
constexpr float kReferenceStep = 1.0f / 30.0f;
constexpr float kMinimumDelta = 0.00001f;
constexpr std::array<std::byte, 13> kChainFixedStepPrefixTemplate{
    std::byte{0xD9}, std::byte{0x05}, std::byte{0x2C}, std::byte{0x19},
    std::byte{0xD6}, std::byte{0x00}, std::byte{0x83}, std::byte{0xEC},
    std::byte{0x08}, std::byte{0xD9}, std::byte{0x5C}, std::byte{0x24},
    std::byte{0x04},
};

using ChainWrapperFn = void (__thiscall*)(void* chain, float delta);
using ChainStepFn = void (__thiscall*)(void* chain, float delta);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_detour;
ChainWrapperFn g_originalWrapper{};
std::atomic_bool g_installed{false};
std::atomic_bool g_correct{false};

void __fastcall ChainFixedStepHook(void* chain, void*, float delta) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.chain-fixed-step-total");
    ChainWrapperFn original = g_originalWrapper;
    if (!original) return;

    const bool correct = g_correct.load(std::memory_order_relaxed);
    if (!correct || !chain || !std::isfinite(delta) ||
        delta <= kMinimumDelta || delta >= kReferenceStep) {
        original(chain, delta);
        return;
    }

    void** vtable = *static_cast<void***>(chain);
    if (!vtable || !vtable[9]) {
        original(chain, delta);
        return;
    }

    // ChainInstance already handles fractional deltas, so bypass the rounding wrapper
    const auto step = reinterpret_cast<ChainStepFn>(vtable[9]);
    step(chain, delta);
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::FramePacing)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII-2 chain timing disabled: main image is unavailable");
        return TRUE;
    }

    const auto targetRange = image->At(
        kChainFixedStepRva, kChainFixedStepPrefixTemplate.size());
    const auto relocatedConstant = image->Address32(
        kChainStepConstantRva, sizeof(float));
    if (!targetRange || !relocatedConstant) {
        LogWarning("XIII-2 chain timing disabled: RVA contract is outside the image or x86 address space");
        return TRUE;
    }
    std::byte* target = const_cast<std::byte*>(targetRange->data());
    std::array<std::byte, kChainFixedStepPrefixTemplate.size()> expected =
        kChainFixedStepPrefixTemplate;
    std::memcpy(expected.data() + 2u, &*relocatedConstant,
                sizeof(*relocatedConstant));
    if (std::memcmp(target, expected.data(), expected.size()) != 0) {
        LogWarning("XIII-2 chain timing disabled: fixed-step contract mismatch");
        return TRUE;
    }

    if (!g_detour.Install(
            "ff13-2-chain-fixed-step", target,
            reinterpret_cast<void*>(&ChainFixedStepHook),
            expected,
            reinterpret_cast<void**>(&g_originalWrapper))) {
        LogWarning("XIII-2 chain timing disabled: detour ownership failed");
        return TRUE;
    }

    g_installed.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void Pump() {
    if (!g_installed.load(std::memory_order_acquire)) return;

    const settings::PerformanceSettings profile = settings::ActivePerformance();
    const bool correct = profile.highFpsCorrectness &&
        (profile.frameRateLimit == 0u || profile.frameRateLimit > 30u) &&
        frame_pacing::IsActive();
    const bool previous = g_correct.load(std::memory_order_relaxed);
    if (previous != correct) {
        g_correct.store(correct, std::memory_order_release);
        Log("XIII-2 chain substep correction %s",
            correct ? "active" : "inactive");
    }
}

} // namespace novafix::game::xiii2_chain_timing
