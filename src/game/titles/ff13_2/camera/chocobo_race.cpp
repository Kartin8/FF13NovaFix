#include "game/titles/ff13_2/camera/chocobo_race.h"

#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/shared/timing/frame_pacing.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace novafix::game::xiii2_chocobo_race {
namespace {

constexpr std::uint32_t kRaceCameraRva = 0x002C1AF0u;
constexpr std::uint32_t kTransitionConstructorRva = 0x0053F540u;
constexpr std::size_t kTransitionDurationOffset = 160u;
constexpr std::size_t kTransitionCancelOffset = 52u;
constexpr float kRaceTransitionDuration = 500.0f;
constexpr float kDurationTolerance = 0.5f;
constexpr ULONGLONG kMaximumRequestGapMs = 100u;

constexpr std::array<std::uint8_t, 12> kRaceCameraPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
    0x81, 0xEC, 0xF4, 0x01, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kTransitionConstructorPrefix{
    0x56, 0x8B, 0xF1, 0xE8, 0x98, 0xFF, 0xFF, 0xFF};

using RaceCameraFn = int (__thiscall*)(void*, int);
using TransitionConstructorFn =
    float* (__thiscall*)(void*, int, int, float*, int);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_raceCameraDetour;
patch::Detour32 g_transitionConstructorDetour;
RaceCameraFn g_originalRaceCamera{};
TransitionConstructorFn g_originalTransitionConstructor{};
std::atomic_bool g_installed{false};
std::atomic_bool g_correct{false};
std::atomic<void*> g_latestTransition{};
std::atomic<ULONGLONG> g_latestTransitionTick{};
thread_local bool g_insideRaceCamera{};

void ClearLatestTransition() {
    g_latestTransition.store(nullptr, std::memory_order_release);
    g_latestTransitionTick.store(0u, std::memory_order_release);
}

int __fastcall RaceCameraHook(void* race, void*, int delta) {
    RaceCameraFn original = g_originalRaceCamera;
    if (!original) return 0;
    if (!g_correct.load(std::memory_order_acquire)) {
        return original(race, delta);
    }

    const bool previous = g_insideRaceCamera;
    g_insideRaceCamera = true;
    const int result = original(race, delta);
    g_insideRaceCamera = previous;
    return result;
}

float* __fastcall TransitionConstructorHook(
    void* transition, void*, int cameraRecord, int activeCount,
    float* parameters, int enabled) {
    TransitionConstructorFn original = g_originalTransitionConstructor;
    if (!original) return nullptr;

    float* result = original(
        transition, cameraRecord, activeCount, parameters, enabled);
    if (!result || !transition || !g_insideRaceCamera ||
        !g_correct.load(std::memory_order_acquire)) {
        return result;
    }

    float duration{};
    std::memcpy(&duration,
                static_cast<const std::byte*>(transition) +
                    kTransitionDurationOffset,
                sizeof(duration));
    if (!std::isfinite(duration) ||
        std::fabs(duration - kRaceTransitionDuration) > kDurationTolerance) {
        ClearLatestTransition();
        return result;
    }

    const ULONGLONG now = GetTickCount64();
    void* previous = g_latestTransition.exchange(
        transition, std::memory_order_acq_rel);
    const ULONGLONG previousTick = g_latestTransitionTick.exchange(
        now, std::memory_order_acq_rel);
    if (!previous || previous == transition || previousTick == 0u ||
        now < previousTick || now - previousTick > kMaximumRequestGapMs) {
        return result;
    }

    auto* cancel = reinterpret_cast<volatile LONG*>(
        static_cast<std::byte*>(previous) + kTransitionCancelOffset);
    if (patch::memory::IsReadable(
            const_cast<const LONG*>(cancel), sizeof(*cancel))) {
        InterlockedExchange(cancel, 1);
    }
    return result;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::FramePacing)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return TRUE;
    const auto raceCamera = image->At(
        kRaceCameraRva, kRaceCameraPrefix.size());
    const auto transitionConstructor = image->At(
        kTransitionConstructorRva, kTransitionConstructorPrefix.size());
    if (!raceCamera || !transitionConstructor) return TRUE;

    if (!g_raceCameraDetour.Install(
            "ff13-2-chocobo-race-camera",
            const_cast<std::byte*>(raceCamera->data()),
            reinterpret_cast<void*>(&RaceCameraHook),
            std::as_bytes(std::span{kRaceCameraPrefix}),
            reinterpret_cast<void**>(&g_originalRaceCamera))) {
        return TRUE;
    }
    if (!g_transitionConstructorDetour.Install(
            "ff13-2-chocobo-race-transition",
            const_cast<std::byte*>(transitionConstructor->data()),
            reinterpret_cast<void*>(&TransitionConstructorHook),
            std::as_bytes(std::span{kTransitionConstructorPrefix}),
            reinterpret_cast<void**>(&g_originalTransitionConstructor))) {
        g_raceCameraDetour.RevertAndClear(g_originalRaceCamera);
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
        frame_pacing::IsHighRefreshPresentationActive();
    const bool previous = g_correct.exchange(correct, std::memory_order_acq_rel);
    if (previous && !correct) ClearLatestTransition();
}

} // namespace novafix::game::xiii2_chocobo_race
