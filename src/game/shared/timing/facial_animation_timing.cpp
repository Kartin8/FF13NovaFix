#include "game/shared/timing/facial_animation_timing.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/shared/timing/frame_pacing.h"
#include "game/core/game_profile.h"
#include "game/shared/timing/facial_animation_policy.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "performance/frame_metrics.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace novafix::game::facial_animation_timing {
namespace {

constexpr std::uint32_t kFf13FacialUpdateRva = 0x7A4F60u;
constexpr std::array<std::byte, 9> kFf13FacialUpdatePrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x81}, std::byte{0xEC}, std::byte{0x20},
    std::byte{0x02}, std::byte{0x00}, std::byte{0x00},
};
constexpr std::uint32_t kXiii2FacialUpdateRva = 0x5C08E0u;
constexpr std::uint32_t kXiii2SecurityCookieRva = 0x1FA90DCu;
constexpr std::array<std::byte, 4> kXiii2FacialPrefixStart{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x24}, std::byte{0xA1},
};
constexpr std::array<std::byte, 9> kXiii2FacialPrefixContinuation{
    std::byte{0x33}, std::byte{0xC4}, std::byte{0x89},
    std::byte{0x44}, std::byte{0x24}, std::byte{0x20},
    std::byte{0x56}, std::byte{0x8B}, std::byte{0xF1},
};
constexpr std::uint32_t kLrFacialUpdateRva = 0x596AB0u;
constexpr std::uint32_t kLrSecurityCookieRva = 0x1F482E8u;
constexpr std::array<std::byte, 7> kLrFacialPrefixStart{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x28}, std::byte{0xA1},
};
constexpr std::array<std::byte, 8> kLrFacialPrefixContinuation{
    std::byte{0x33}, std::byte{0xC5}, std::byte{0x89}, std::byte{0x45},
    std::byte{0xFC}, std::byte{0x8B}, std::byte{0x45}, std::byte{0x08},
};
constexpr std::size_t kTrackedControllers = 128;


using FacialUpdateFn = void (__thiscall*)(void* controller, const std::int32_t* timing);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_detour;
FacialUpdateFn g_originalUpdate = nullptr;
std::atomic_bool g_available{false};
std::atomic_int32_t g_expectedTicks{0};
std::int32_t g_referenceTicks =
    facial_animation_policy::kFacialReference60HzTicks;
bool g_useXiii2WallDelta{};
std::size_t g_transitionRateOffset = 14u;


enum class FacialLayout {
    Ff13,
    Xiii2,
    LightningReturns,
};

struct SmoothingLayout {
    std::size_t smoothedA{};
    std::size_t outputA{};
    std::size_t targetA{};
    std::size_t smoothedB{};
    std::size_t outputB{};
    std::size_t targetB{};
};

constexpr SmoothingLayout kXiiiSmoothing{
    304u, 284u, 308u,
    312u, 288u, 316u,
};
constexpr SmoothingLayout kLrSmoothing{
    348u, 288u, 352u,
    356u, 292u, 360u,
};

FacialLayout g_layout = FacialLayout::Ff13;
SmoothingLayout g_smoothing = kXiiiSmoothing;

struct FacialContract {
    std::uint32_t updateRva{};
    std::int32_t referenceTicks{};
    bool useXiii2WallDelta{};
    FacialLayout layout{};
    std::size_t transitionRateOffset{};
    const char* title{};
};

std::optional<FacialContract> ContractFor(Title title) {
    switch (title) {
    case Title::FinalFantasyXIII:
        return FacialContract{kFf13FacialUpdateRva,
                              facial_animation_policy::kReference30HzTicks,
                              false, FacialLayout::Ff13, 14u, "XIII"};
    case Title::FinalFantasyXIII2:
        return FacialContract{kXiii2FacialUpdateRva,
                              facial_animation_policy::kXiii2Reference30HzTicks,
                              true, FacialLayout::Xiii2, 14u, "XIII-2"};
    case Title::LightningReturns:
        return FacialContract{kLrFacialUpdateRva,
                              facial_animation_policy::kFacialReference60HzTicks,
                              false, FacialLayout::LightningReturns,
                              10u, "LR"};
    case Title::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::vector<std::byte>> ValidatedPrefix(
    const ImageView& image, const FacialContract& contract) {
    const std::span<const std::byte> bytes = image.Bytes();
    if (contract.updateRva >= bytes.size()) return std::nullopt;
    const std::byte* address = bytes.data() + contract.updateRva;

    if (contract.layout == FacialLayout::Ff13) {
        if (!image.Contains(address, kFf13FacialUpdatePrefix.size()) ||
            std::memcmp(address, kFf13FacialUpdatePrefix.data(),
                        kFf13FacialUpdatePrefix.size()) != 0) {
            return std::nullopt;
        }
        return std::vector<std::byte>(kFf13FacialUpdatePrefix.begin(),
                                      kFf13FacialUpdatePrefix.end());
    }

    if (contract.layout == FacialLayout::LightningReturns) {
        constexpr std::size_t kLivePrefixSize = 11u;
        constexpr std::size_t kContractSize =
            kLivePrefixSize + kLrFacialPrefixContinuation.size();
        if (!image.Contains(address, kContractSize) ||
            std::memcmp(address, kLrFacialPrefixStart.data(),
                        kLrFacialPrefixStart.size()) != 0 ||
            std::memcmp(address + kLivePrefixSize,
                        kLrFacialPrefixContinuation.data(),
                        kLrFacialPrefixContinuation.size()) != 0) {
            return std::nullopt;
        }

        std::uint32_t relocatedCookie{};
        std::memcpy(&relocatedCookie, address + 7u, sizeof(relocatedCookie));
        const auto expectedCookie = image.Address32(
            kLrSecurityCookieRva, sizeof(std::uint32_t));
        if (!expectedCookie || relocatedCookie != *expectedCookie) {
            return std::nullopt;
        }
        return std::vector<std::byte>(address, address + kLivePrefixSize);
    }

    constexpr std::size_t kLivePrefixSize = 8u;
    constexpr std::size_t kContractSize =
        kLivePrefixSize + kXiii2FacialPrefixContinuation.size();
    if (!image.Contains(address, kContractSize) ||
        std::memcmp(address, kXiii2FacialPrefixStart.data(),
                    kXiii2FacialPrefixStart.size()) != 0 ||
        std::memcmp(address + kLivePrefixSize,
                    kXiii2FacialPrefixContinuation.data(),
                    kXiii2FacialPrefixContinuation.size()) != 0) {
        return std::nullopt;
    }

    std::uint32_t relocatedCookie{};
    std::memcpy(&relocatedCookie, address + 4u, sizeof(relocatedCookie));
    const auto expectedCookie = image.Address32(
        kXiii2SecurityCookieRva, sizeof(std::uint32_t));
    if (!expectedCookie || relocatedCookie != *expectedCookie) {
        return std::nullopt;
    }

    // A1 carries an ASLR-relocated absolute operand. Copy the validated live
    // instruction into the trampoline instead of comparing it with preferred
    // image-base bytes
    return std::vector<std::byte>(address, address + kLivePrefixSize);
}

struct ControllerHistory {
    void* controller{};
    std::array<std::uint8_t, 3> lastOutput{};
    bool valid{};
};

thread_local std::array<ControllerHistory, kTrackedControllers> g_controllerHistory{};
thread_local std::size_t g_nextControllerHistory{};
thread_local ControllerHistory* g_lastControllerHistory{};

float ReadFloat(const void* object, std::size_t offset) {
    float value{};
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}

void WriteFloat(void* object, std::size_t offset, float value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(value));
}

std::array<std::uint8_t, 3> ReadTransitionRates(const void* controller) {
    std::array<std::uint8_t, 3> result{};
    std::memcpy(result.data(),
                static_cast<const std::byte*>(controller) +
                    g_transitionRateOffset,
                result.size());
    return result;
}

ControllerHistory& HistoryFor(void* controller) {
    if (g_lastControllerHistory &&
        g_lastControllerHistory->controller == controller) {
        return *g_lastControllerHistory;
    }
    for (ControllerHistory& history : g_controllerHistory) {
        if (history.controller == controller) {
            g_lastControllerHistory = &history;
            return history;
        }
    }
    for (ControllerHistory& history : g_controllerHistory) {
        if (!history.controller) {
            history.controller = controller;
            g_lastControllerHistory = &history;
            return history;
        }
    }
    ControllerHistory& history =
        g_controllerHistory[g_nextControllerHistory++ % g_controllerHistory.size()];
    history = {};
    history.controller = controller;
    g_lastControllerHistory = &history;
    return history;
}

bool NormalizeTransitionRates(
    void* controller, const std::array<std::uint8_t, 3>& before,
    std::int32_t gameTicks, std::int32_t referenceTicks) {
    ControllerHistory& history = HistoryFor(controller);
    std::array<std::uint8_t, 3> after = ReadTransitionRates(controller);

    // If the function returned before refreshing its morph parameters, keep
    // the values that NovaFix already normalized instead of scaling twice
    if (history.valid && before == history.lastOutput && after == before) {
        return false;
    }

    const std::array<std::uint8_t, 3> unscaled = after;
    for (std::uint8_t& value : after) {
        value = facial_animation_policy::ScaleLinearTransitionByte(
            value, gameTicks, referenceTicks);
    }
    std::memcpy(static_cast<std::byte*>(controller) + g_transitionRateOffset,
                after.data(), after.size());
    history.lastOutput = after;
    history.valid = true;
    return after != unscaled;
}

bool NormalizeSmoothing(void* controller, const SmoothingLayout& layout,
                        float beforeA, float beforeB,
                        std::int32_t gameTicks,
                        std::int32_t referenceTicks) {
    const float afterA = ReadFloat(controller, layout.smoothedA);
    const float afterB = ReadFloat(controller, layout.smoothedB);
    const float alpha = facial_animation_policy::TimeCorrectedAlpha(
        0.2f, gameTicks, referenceTicks);
    bool changed = false;

    if (std::isfinite(beforeA) && std::isfinite(afterA) && afterA != beforeA) {
        const float target = ReadFloat(controller, layout.targetA);
        if (std::isfinite(target)) {
            const float corrected = beforeA + (target - beforeA) * alpha;
            WriteFloat(controller, layout.smoothedA, corrected);
            WriteFloat(controller, layout.outputA, corrected);
            changed = true;
        }
    }
    if (std::isfinite(beforeB) && std::isfinite(afterB) && afterB != beforeB) {
        const float target = ReadFloat(controller, layout.targetB);
        if (std::isfinite(target)) {
            const float corrected = beforeB + (target - beforeB) * alpha;
            WriteFloat(controller, layout.smoothedB, corrected);
            WriteFloat(controller, layout.outputB, corrected);
            changed = true;
        }
    }
    return changed;
}

void __fastcall FacialUpdateHook(void* controller, void*, const std::int32_t* timing) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.shared.facial-update-total");
    FacialUpdateFn original = g_originalUpdate;
    if (!original) return;
    if (!timing) {
        original(controller, timing);
        return;
    }
    if (!controller) {
        original(controller, timing);
        return;
    }

    const std::int32_t rawTicks = timing[0];
    const bool useWallDelta = g_useXiii2WallDelta &&
        frame_pacing::IsHighRefreshPresentationActive();
    const std::uint32_t wallTicks = useWallDelta
        ? frame_pacing::AppliedPresentationDeltaTicks() : 0u;
    const std::int32_t gameTicks =
        facial_animation_policy::ResolveFacialTicks(
        rawTicks, wallTicks, useWallDelta, g_referenceTicks);
    const std::int32_t expectedTicks = g_expectedTicks.load(std::memory_order_relaxed);
    const bool normalize = facial_animation_policy::ShouldNormalize(
        gameTicks, expectedTicks, g_referenceTicks);
    if (!normalize) {
        original(controller, timing);
        return;
    }

    const std::array<std::uint8_t, 3> transitionRates =
        ReadTransitionRates(controller);
    const float smoothedA = ReadFloat(controller, g_smoothing.smoothedA);
    const float smoothedB = ReadFloat(controller, g_smoothing.smoothedB);
    original(controller, timing);

    if (g_layout == FacialLayout::LightningReturns) {
        // LR uses this bit to request an intentional one-frame snap to the
        // target morph. Turning that back into a blend breaks face changes
        std::uint32_t flags{};
        std::memcpy(&flags, static_cast<const std::byte*>(controller) + 320u,
                    sizeof(flags));
        if ((flags & 0x10u) != 0u) return;
    }
    NormalizeTransitionRates(
        controller, transitionRates, gameTicks, g_referenceTicks);
    NormalizeSmoothing(
        controller, g_smoothing, smoothedA, smoothedB,
        gameTicks, g_referenceTicks);
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    const auto contract = ContractFor(CurrentProfile().title);
    if (!image || !contract ||
        !Supports(Capability::HighFpsFacialTiming)) {
        LogWarning("High-FPS animation fix disabled: unsupported executable image");
        return TRUE;
    }

    const auto prefix = ValidatedPrefix(*image, *contract);
    if (!prefix) {
        LogWarning("High-FPS animation fix disabled: %s facial update contract mismatch",
            contract->title);
        return TRUE;
    }

    const auto updateRange = image->At(contract->updateRva, prefix->size());
    if (!updateRange) {
        LogWarning("High-FPS animation fix disabled: %s facial RVA outside image",
            contract->title);
        return TRUE;
    }
    std::byte* address = const_cast<std::byte*>(updateRange->data());
    g_referenceTicks = contract->referenceTicks;
    g_useXiii2WallDelta = contract->useXiii2WallDelta;
    g_layout = contract->layout;
    g_transitionRateOffset = contract->transitionRateOffset;
    g_smoothing = contract->layout == FacialLayout::LightningReturns
        ? kLrSmoothing : kXiiiSmoothing;
    if (!g_detour.Install("high-fps-facial-timing",
                          address,
                          reinterpret_cast<void*>(&FacialUpdateHook),
                          *prefix,
                          reinterpret_cast<void**>(&g_originalUpdate))) {
        LogWarning("High-FPS animation fix disabled: %s facial update detour failed",
            contract->title);
        return TRUE;
    }

    g_available.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void Pump() {
    if (!g_available.load(std::memory_order_acquire)) return;

    const settings::PerformanceSettings settings = settings::ActivePerformance();
    std::int32_t expectedTicks = 0;
    if (settings.highFpsCorrectness &&
        CurrentProfile().title == Title::LightningReturns) {
        // LR facial correction is independent of NovaFix's presentation and
        // limiter settings. Drive it only from the measured render interval
        expectedTicks = facial_animation_policy::ExpectedTicks(
            performance::CurrentFrameMilliseconds(), 0u);
    } else if (settings.highFpsCorrectness &&
               (settings.frameRateLimit == 0 || settings.frameRateLimit > 30)) {
        expectedTicks = facial_animation_policy::ExpectedTicks(
            performance::CurrentFrameMilliseconds(),
            performance::CurrentTargetRate());
    }
    if (g_expectedTicks.load(std::memory_order_relaxed) != expectedTicks) {
        g_expectedTicks.store(expectedTicks, std::memory_order_release);
    }
}

} // namespace novafix::game::facial_animation_timing
