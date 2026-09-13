#include "game/titles/ff13_2/animation/root_motion.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/titles/ff13_2/timing/frame_timing_policy.h"
#include "game/shared/timing/frame_pacing.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::xiii2_root_motion {
namespace {

// UPDATE_ROOT_MOVE consumes the global 10010-tick (29.97 Hz) quantum directly
// Once presentation is decoupled it runs every rendered frame, advancing
// motion too quickly while scene and camera timing remain fixed
constexpr std::uint32_t kRootMoveTaskRva = 0x007F1800u;
constexpr std::uint32_t kGlobalQuantumRva = 0x01FA8B30u;
// Keep the complete seven-byte F6 /0 instruction. Ending this contract before
// its 0x80 immediate splits the instruction in the trampoline and raises
// STATUS_PRIVILEGED_INSTRUCTION as soon as UPDATE_ROOT_MOVE executes
constexpr std::array<std::uint8_t, 10> kRootMoveTaskPrefix{
    0x56, 0x8B, 0xF1, 0xF6, 0x86, 0x71, 0x02, 0x00, 0x00, 0x80};

using RootMoveTaskFn = void (__thiscall*)(void*, const std::uint32_t*);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_detour;
RootMoveTaskFn g_original{};
const std::uint32_t* g_authoredQuantum{};
std::atomic_bool g_installed{false};
std::atomic_bool g_correct{false};

void __fastcall RootMoveTaskHook(void* owner, void*,
                                 const std::uint32_t* taskTicks) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.root-motion-total");
    RootMoveTaskFn original = g_original;
    if (!original) return;

    const bool ownsAuthoredQuantum = taskTicks == g_authoredQuantum;
    const bool correctionRequested =
        g_correct.load(std::memory_order_acquire) && ownsAuthoredQuantum;
    if (!correctionRequested) {
        original(owner, taskTicks);
        return;
    }

    const std::uint32_t rawTicks = *g_authoredQuantum;
    const std::uint32_t wallTicks =
        frame_pacing::AppliedPresentationDeltaTicks();
    const std::uint32_t appliedTicks =
        xiii2_frame_timing_policy::Xiii2RootMotionTicks(
            rawTicks, wallTicks,
            frame_pacing::IsHighRefreshPresentationActive(),
            ownsAuthoredQuantum);
    const bool corrected = appliedTicks != rawTicks;

    if (corrected) {
        original(owner, &appliedTicks);
    } else {
        original(owner, taskTicks);
    }
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::FramePacing)) {
        return TRUE;
    }
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII-2 root-motion correction disabled: main image unavailable");
        return TRUE;
    }
    const auto taskRange = image->At(
        kRootMoveTaskRva, kRootMoveTaskPrefix.size());
    const auto quantumRange = image->At(
        kGlobalQuantumRva, sizeof(std::uint32_t));
    if (!taskRange || !quantumRange ||
        std::memcmp(taskRange->data(), kRootMoveTaskPrefix.data(),
                    kRootMoveTaskPrefix.size()) != 0) {
        LogWarning("XIII-2 root-motion correction disabled: executable contract mismatch");
        return TRUE;
    }
    std::byte* task = const_cast<std::byte*>(taskRange->data());
    if (!g_detour.Install(
            "ff13-2-root-motion-delta", task,
            reinterpret_cast<void*>(&RootMoveTaskHook),
            std::as_bytes(std::span{kRootMoveTaskPrefix}),
            reinterpret_cast<void**>(&g_original))) {
        LogWarning("XIII-2 root-motion correction disabled: detour ownership failed");
        return TRUE;
    }
    g_authoredQuantum = reinterpret_cast<const std::uint32_t*>(
        quantumRange->data());
    g_installed.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void Pump() {
    if (!g_installed.load(std::memory_order_acquire)) return;

    const settings::PerformanceSettings profile =
        settings::ActivePerformance();
    const bool correct = profile.highFpsCorrectness &&
        (profile.frameRateLimit == 0u || profile.frameRateLimit > 30u) &&
        frame_pacing::IsHighRefreshPresentationActive();
    const bool previous = g_correct.load(std::memory_order_relaxed);
    if (previous != correct) {
        g_correct.store(correct, std::memory_order_release);
        Log("XIII-2 root-motion delta correction %s",
            correct ? "active" : "inactive");
    }

}

} // namespace novafix::game::xiii2_root_motion
