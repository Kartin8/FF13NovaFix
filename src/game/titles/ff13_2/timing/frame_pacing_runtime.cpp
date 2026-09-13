#include "game/titles/ff13_2/timing/frame_pacing_runtime.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/shared/timing/frame_pacing.h"
#include "game/titles/ff13_2/timing/frame_timing_policy.h"
#include "game/shared/timing/frame_pacing_state.h"
#include "game/titles/ff13_2/timing/delta_guard.h"
#include "overlay/overlay.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"
#include "performance/frame_limiter.h"
#include "performance/frame_metrics.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace novafix::game::xiii2_frame_pacing_runtime {
namespace {

constexpr float kAuthoredSimulationRate = 29.97003f;
constexpr ULONGLONG kWaitPatchRetryIntervalMs = 30'000u;
constexpr std::uint32_t kStatePointerRva = 0x4D67208u;
constexpr std::uint32_t kSetterRva = 0x802610u;
constexpr std::uint32_t kDeltaRva = 0x802660u;
constexpr std::uint32_t kFrameSignalRva = 0x6417E0u;
constexpr std::uint32_t kWaitTimeoutRva = 0x8027DAu;
constexpr std::uint32_t kGameMainPointerRva = 0x1FABAA8u;
constexpr std::uint32_t kGameMainVtableRva = 0x9546F0u;
constexpr std::uint32_t kFrameSourceVtableRva = 0x1ED86E8u;
constexpr std::uint32_t kMovieOpenRva = 0x6204D0u;
constexpr std::uint32_t kVeffUpdateRva = 0x4DEAC0u;

constexpr std::array<std::uint8_t, 14> kSetterPrefix{
    0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04, 0xF3,
    0x0F, 0x11, 0x41, 0x04, 0xC2, 0x04, 0x00,
};
constexpr std::array<std::uint8_t, 9> kDeltaPrefix{
    0x8D, 0x51, 0x2C, 0x56, 0xB8, 0x01, 0x00, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 5> kFrameSignalPrefix{
    0xE9, 0x8B, 0xB4, 0x1D, 0x00,
};

struct DeltaCallSite {
    std::uint32_t rva;
    std::array<std::uint8_t, 5> expected;
};

// C02660's retry jumps into its prologue at C02664. Redirect validated callers;
// an entry detour would overwrite the retry target
constexpr std::array<DeltaCallSite, 4> kDeltaCallSites{{
    {0x8043C2u, {0xE8, 0x99, 0xE2, 0xFF, 0xFF}},
    {0x804400u, {0xE8, 0x5B, 0xE2, 0xFF, 0xFF}},
    {0x804E8Au, {0xE8, 0xD1, 0xD7, 0xFF, 0xFF}},
    {0x8050EAu, {0xE8, 0x71, 0xD5, 0xFF, 0xFF}},
}};

struct FrameSignalCallSite {
    std::uint32_t rva;
    std::array<std::uint8_t, 5> expected;
};

// Both C04C20 branches execute this signal immediately after the render-ring
// drain and immediately before C02660 reads the simulation delta.  Waiting in
// the redirected call preserves Steam's renderer synchronization while placing
// the absolute cadence boundary at the same phase used by the Store build
constexpr std::array<FrameSignalCallSite, 2> kFrameSignalCallSites{{
    {0x804E7Au, {0xE8, 0x61, 0xC9, 0xE3, 0xFF}},
    {0x8050DAu, {0xE8, 0x01, 0xC7, 0xE3, 0xFF}},
}};

constexpr std::array<std::uint8_t, 2> kWaitInfinite{
    0x6A, 0xFF,
};
// Keep a 1-ms event wait: zero busy-spins against movie/decode threads,
// while the original timeout couples presentation to 29.97 Hz
constexpr std::array<std::uint8_t, 2> kWaitYield{
    0x6A, 0x01,
};
constexpr std::array<std::uint8_t, 8> kMovieOpenPrefix{
    0x83, 0xEC, 0x20, 0x53, 0x56, 0x8B, 0xF1, 0x57,
};
constexpr std::array<std::uint8_t, 7> kVeffUpdatePrefix{
    0xD9, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x50,
};
constexpr char kMenuHighlightResource[] = "m_remenu_con";

using SetFrameRateFn = void (__thiscall*)(void* state, float frameRate);
using ReadDeltaFn = std::uint32_t* (__thiscall*)(void* state,
                                                 std::uint32_t* output);
using FrameSignalFn = void (__cdecl*)();
using MovieOpenFn = void (__thiscall*)(
    void* player, std::uint32_t source, std::uint32_t path,
    std::uint32_t pathSize, std::uint8_t memorySource, std::uint8_t loop,
    std::uint32_t audioTrack, std::uint32_t rangeStartLow,
    std::uint32_t rangeStartHigh, std::uint32_t rangeEndLow,
    std::uint32_t rangeEndHigh);
using VeffUpdateFn = int (__thiscall*)(void* effect,
                                      float deltaMilliseconds);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_supported{};
float** g_frameStatePointer{};
float* g_frameState{};
void** g_gameMainPointer{};
void* g_gameMainVtable{};
void* g_frameSourceVtable{};
float* g_changedFrameSourceRate{};
float g_originalFrameSourceRate{};
float g_appliedFrameSourceRate{};
std::unique_ptr<patch::Transaction> g_waitPatch;
std::unique_ptr<patch::Transaction> g_deltaCallsPatch;
std::unique_ptr<patch::Transaction> g_frameSignalCallsPatch;
patch::Detour32 g_setterDetour;
patch::Detour32 g_movieOpenDetour;
patch::Detour32 g_veffUpdateDetour;
SetFrameRateFn g_originalSetter{};
ReadDeltaFn g_originalReadDelta{};
FrameSignalFn g_originalFrameSignal{};
MovieOpenFn g_originalMovieOpen{};
VeffUpdateFn g_originalVeffUpdate{};
std::atomic_bool g_movieOpenPending{};
std::atomic<void*> g_pendingMoviePlayer{};
std::atomic_bool g_loggedGameBoundaryPacingActive{};
std::atomic_int32_t g_observedFrameSourceMode{
    (std::numeric_limits<std::int32_t>::min)()};
std::atomic_uint32_t g_deltaGeneration{1u};
thread_local std::uint32_t g_seenDeltaGeneration{};
thread_local LARGE_INTEGER g_lastPresentationCounter{};
LARGE_INTEGER g_counterFrequency{};
SRWLOCK g_stateLock = SRWLOCK_INIT;
ULONGLONG g_waitPatchRetryAt{};
bool g_waitPatchApplyFailureLogged{};
bool g_waitPatchRevertFailureLogged{};

bool IsReadable(const void* address, std::size_t size) {
    if (!address || size == 0u) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionEnd =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
        memory.RegionSize;
    return begin <= regionEnd && size <= regionEnd - begin;
}

void WriteFloat(float* address, float value) {
    const LONG bits = std::bit_cast<LONG>(value);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(address), bits);
}

bool SameFloat(float left, float right) {
    return std::bit_cast<std::uint32_t>(left) ==
        std::bit_cast<std::uint32_t>(right);
}

struct FrameSourceView {
    void* object{};
    float* rate{};
    std::int32_t mode{};
};

struct FrameSourceCache {
    void* gameMain{};
    void* source{};
    float* rate{};
    std::int32_t* mode{};
    bool valid{};
};

thread_local FrameSourceCache g_frameSourceCache{};

std::optional<FrameSourceView> CurrentFrameSource() {
    if (!g_gameMainPointer || !g_gameMainVtable || !g_frameSourceVtable) {
        return std::nullopt;
    }
    void* gameMain = *g_gameMainPointer;
    FrameSourceCache& cache = g_frameSourceCache;
    if (cache.gameMain != gameMain || !cache.valid) {
        cache = {};
        cache.gameMain = gameMain;
        if (!gameMain) return std::nullopt;

        constexpr std::size_t kFrameSourceObjectEnd =
            748u + sizeof(std::int32_t);
        auto* bytes = static_cast<std::byte*>(gameMain);
        void* source = bytes + 8u;
        auto* rate = reinterpret_cast<float*>(bytes + 12u);
        auto* mode = reinterpret_cast<std::int32_t*>(bytes + 748u);
        if (!IsReadable(gameMain, kFrameSourceObjectEnd) ||
            !patch::memory::IsWritable(rate, sizeof(*rate)) ||
            *reinterpret_cast<void**>(gameMain) != g_gameMainVtable ||
            *reinterpret_cast<void**>(source) != g_frameSourceVtable) {
            return std::nullopt;
        }
        cache.source = source;
        cache.rate = rate;
        cache.mode = mode;
        cache.valid = true;
    }
    if (!cache.valid) return std::nullopt;
    return FrameSourceView{cache.source, cache.rate, *cache.mode};
}

void RememberFrameSourceChange(float* rate, float original) {
    if (g_changedFrameSourceRate != rate) {
        g_changedFrameSourceRate = rate;
        g_originalFrameSourceRate = original;
    }
}

void ForgetFrameSourceChange() {
    g_changedFrameSourceRate = nullptr;
    g_originalFrameSourceRate = 0.0f;
    g_appliedFrameSourceRate = 0.0f;
}

void RestoreFrameSource() {
    float* rate = g_changedFrameSourceRate;
    if (rate && patch::memory::IsWritable(rate, sizeof(float)) &&
        SameFloat(*rate, g_appliedFrameSourceRate)) {
        WriteFloat(rate, g_originalFrameSourceRate);
    }
    ForgetFrameSourceChange();
}

void ClearMovieOpenPending() {
    g_movieOpenPending.store(false, std::memory_order_release);
    g_pendingMoviePlayer.store(nullptr, std::memory_order_release);
}

bool IsMovieOpenPending() {
    if (!g_movieOpenPending.load(std::memory_order_acquire)) return false;
    const auto* player = static_cast<const std::byte*>(
        g_pendingMoviePlayer.load(std::memory_order_acquire));
    constexpr std::size_t kOpenAcceptedOffset = 205u;
    constexpr std::size_t kFirstFrameReadyOffset = 206u;
    if (!player ||
        !IsReadable(player + kOpenAcceptedOffset,
                    sizeof(std::uint8_t) * 2u)) {
        ClearMovieOpenPending();
        return false;
    }
    const auto* state =
        reinterpret_cast<const volatile std::uint8_t*>(player);
    if (!state[kOpenAcceptedOffset] || state[kFirstFrameReadyOffset]) {
        ClearMovieOpenPending();
        return false;
    }
    return true;
}

void ApplyFrameSourcePolicy(
    const settings::PerformanceSettings& settings,
    bool& presentationMode) {
    presentationMode = false;
    const auto source = CurrentFrameSource();
    if (!source) return;
    presentationMode = xiii2_frame_timing_policy::Xiii2CanDecouplePresentation(
        source->mode);
    const std::int32_t previousMode =
        g_observedFrameSourceMode.load(std::memory_order_relaxed);
    if (previousMode != source->mode) {
        g_observedFrameSourceMode.store(
            source->mode, std::memory_order_relaxed);
        Log("XIII-2 frame-source mode: %d -> %d source=%.6f",
            previousMode, source->mode, *source->rate);
    }
    const bool ownsCurrentSource =
        g_changedFrameSourceRate == source->rate;
    const float original = ownsCurrentSource
        ? g_originalFrameSourceRate : *source->rate;
    const float adjusted = xiii2_frame_timing_policy::Xiii2FrameSourceRate(
        original, source->mode, settings.highFpsCorrectness,
        settings.frameRateLimit);
    if (ownsCurrentSource && SameFloat(adjusted, original)) {
        // Only gameplay owns the saved rate; applying it to movie mode 2 breaks playback
        if (source->mode == 1) RestoreFrameSource();
        else ForgetFrameSourceChange();
        return;
    }
    if (!SameFloat(adjusted, *source->rate)) {
        const bool policyChanged = !ownsCurrentSource ||
            !SameFloat(adjusted, g_appliedFrameSourceRate);
        RememberFrameSourceChange(source->rate, *source->rate);
        g_appliedFrameSourceRate = adjusted;
        WriteFloat(source->rate, adjusted);
        if (policyChanged) {
            Log("XIII-2 authored frame source active: Steam %.6f -> Xbox %.6f mode=%d",
                g_originalFrameSourceRate, adjusted, source->mode);
        }
    }
}

void __fastcall SetFrameRateHook(
    void* state, void*, float frameRate) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.frame-rate-setter-total");
    SetFrameRateFn original = g_originalSetter;
    if (!original) return;

    const settings::PerformanceSettings settings =
        settings::ActivePerformance();
    float forwardedRate = frameRate;
    AcquireSRWLockExclusive(&g_stateLock);
    const auto source = CurrentFrameSource();
    if (source && state == source->object) {
        const bool ownsCurrentSource =
            g_changedFrameSourceRate == source->rate;
        const float requested = ownsCurrentSource && source->mode == 1 &&
            SameFloat(frameRate, g_appliedFrameSourceRate)
            ? g_originalFrameSourceRate : frameRate;
        const float adjusted = xiii2_frame_timing_policy::Xiii2FrameSourceRate(
            requested, source->mode, settings.highFpsCorrectness,
            settings.frameRateLimit);
        if (!SameFloat(adjusted, requested)) {
            RememberFrameSourceChange(source->rate, requested);
            g_appliedFrameSourceRate = adjusted;
        } else if (ownsCurrentSource) {
            ForgetFrameSourceChange();
        }
        forwardedRate = adjusted;
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    original(state, forwardedRate);
}

void __fastcall MovieOpenHook(
    void* player, void*, std::uint32_t source, std::uint32_t path,
    std::uint32_t pathSize, std::uint8_t memorySource, std::uint8_t loop,
    std::uint32_t audioTrack, std::uint32_t rangeStartLow,
    std::uint32_t rangeStartHigh, std::uint32_t rangeEndLow,
    std::uint32_t rangeEndHigh) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.movie-open-total");
    MovieOpenFn original = g_originalMovieOpen;
    if (!original) return;

    const auto* bytes = static_cast<const std::byte*>(player);
    constexpr std::size_t kPlayerActiveOffset = 204u;
    if (bytes &&
        IsReadable(bytes + kPlayerActiveOffset, sizeof(std::uint8_t)) &&
        !*reinterpret_cast<const std::uint8_t*>(
            bytes + kPlayerActiveOffset)) {
        g_pendingMoviePlayer.store(player, std::memory_order_release);
        if (!g_movieOpenPending.exchange(
                true, std::memory_order_acq_rel)) {
            Log("XIII-2 movie open requested: native scheduler wait restored for preparation");
        }
    }
    original(player, source, path, pathSize, memorySource, loop, audioTrack,
             rangeStartLow, rangeStartHigh, rangeEndLow, rangeEndHigh);
}

bool IsMenuHighlightEffect(const void* effect) {
    constexpr std::size_t kResourceNameOffset = 252u;
    const auto* bytes = static_cast<const std::byte*>(effect);
    if (!bytes ||
        !IsReadable(bytes + kResourceNameOffset, sizeof(const char*))) {
        return false;
    }
    const char* resourceName = *reinterpret_cast<const char* const*>(
        bytes + kResourceNameOffset);
    return IsReadable(resourceName, sizeof(kMenuHighlightResource)) &&
        std::memcmp(resourceName, kMenuHighlightResource,
                    sizeof(kMenuHighlightResource)) == 0;
}

int __fastcall VeffUpdateHook(
    void* effect, void*, float deltaMilliseconds) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.veff-update-total");
    VeffUpdateFn original = g_originalVeffUpdate;
    if (!original) return 0;
    if (IsMenuHighlightEffect(effect)) {
        deltaMilliseconds =
            xiii2_frame_timing_policy::Xiii2VeffDeltaMilliseconds(
            deltaMilliseconds,
            frame_pacing::state::appliedPresentationDeltaTicks.load(
                std::memory_order_acquire),
            frame_pacing::state::highRefreshPresentationActive.load(
                std::memory_order_acquire));
    }
    return original(effect, deltaMilliseconds);
}

void __cdecl FrameSignalWithPacingHook() {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.game-boundary-pacing-total");
    if (frame_pacing::state::gameBoundaryPacingActive.load(
            std::memory_order_acquire)) {
        performance::PaceFrame(
            overlay::PresentationWindow(),
            performance::ObservedRefreshRate(),
            performance::FramePacingSite::Xiii2GameBoundary);
    }
    FrameSignalFn original = g_originalFrameSignal;
    if (original) original();
}

std::uint32_t* __fastcall ReadDeltaHook(
    void* state, void*, std::uint32_t* output) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.read-delta-total");
    ReadDeltaFn original = g_originalReadDelta;
    if (!original) return output;
    std::uint32_t* result = original(state, output);
    if (!result) return result;

    std::uint32_t rawTicks{};
    if (!xiii2_delta_guard::TryRead(result, output, rawTicks)) {
        return result;
    }
    std::uint32_t appliedTicks = rawTicks;
    if (frame_pacing::state::ownershipActive.load(
            std::memory_order_acquire)) {
        const std::uint32_t generation =
            g_deltaGeneration.load(std::memory_order_acquire);
        if (g_seenDeltaGeneration != generation) {
            g_lastPresentationCounter.QuadPart = 0;
            g_seenDeltaGeneration = generation;
        }
        if (frame_pacing::state::highRefreshPresentationActive.load(
                std::memory_order_acquire)) {
            LARGE_INTEGER now{};
            if (QueryPerformanceCounter(&now)) {
                if (g_lastPresentationCounter.QuadPart != 0 &&
                    now.QuadPart > g_lastPresentationCounter.QuadPart) {
                    appliedTicks =
                        xiii2_frame_timing_policy::Xiii2WallDeltaTicks(
                        static_cast<std::uint64_t>(
                            now.QuadPart -
                            g_lastPresentationCounter.QuadPart),
                        static_cast<std::uint64_t>(
                            g_counterFrequency.QuadPart));
                    if (!xiii2_delta_guard::TryWrite(
                            result, output, appliedTicks)) {
                        return result;
                    }
                }
                g_lastPresentationCounter = now;
            }
        } else {
            g_lastPresentationCounter.QuadPart = 0;
        }
    }
    frame_pacing::state::appliedPresentationDeltaTicks.store(
        appliedTicks, std::memory_order_release);
    return result;
}

bool InstallDeltaCallRedirects(
    const ImageView& image, std::byte* delta) {
    const std::uint32_t firstRva = kDeltaCallSites.front().rva;
    const std::uint32_t lastRva = kDeltaCallSites.back().rva;
    const std::size_t regionSize =
        static_cast<std::size_t>(lastRva - firstRva) + 5u;
    const auto region = image.At(firstRva, regionSize);
    if (!region) return false;
    std::byte* regionBase = const_cast<std::byte*>(region->data());

    auto transaction = std::make_unique<patch::Transaction>(
        "ff13-2-stable-delta-call-sites",
        std::span<std::byte>(regionBase, regionSize),
        patch::registry::Kind::Detour);
    for (const DeltaCallSite& site : kDeltaCallSites) {
        const auto callRange = image.At(site.rva, site.expected.size());
        if (!callRange ||
            std::memcmp(callRange->data(), site.expected.data(),
                        site.expected.size()) != 0) {
            return false;
        }
        std::byte* call = const_cast<std::byte*>(callRange->data());
        const auto replacement = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(call),
            reinterpret_cast<std::uintptr_t>(&ReadDeltaHook));
        if (!replacement ||
            !transaction->Add(
                site.rva - firstRva,
                std::as_bytes(std::span{site.expected}), *replacement)) {
            return false;
        }
    }

    g_originalReadDelta = reinterpret_cast<ReadDeltaFn>(delta);
    if (transaction->Apply() != patch::ApplyStatus::Applied) {
        g_originalReadDelta = nullptr;
        return false;
    }
    g_deltaCallsPatch = std::move(transaction);
    return true;
}

bool InstallFrameSignalCallRedirects(
    const ImageView& image, std::byte* frameSignal) {
    const std::uint32_t firstRva = kFrameSignalCallSites.front().rva;
    const std::uint32_t lastRva = kFrameSignalCallSites.back().rva;
    const std::size_t regionSize =
        static_cast<std::size_t>(lastRva - firstRva) + 5u;
    const auto region = image.At(firstRva, regionSize);
    if (!region) return false;
    std::byte* regionBase = const_cast<std::byte*>(region->data());

    auto transaction = std::make_unique<patch::Transaction>(
        "ff13-2-store-style-game-boundary-pacing",
        std::span<std::byte>(regionBase, regionSize),
        patch::registry::Kind::Detour);
    for (const FrameSignalCallSite& site : kFrameSignalCallSites) {
        const auto callRange = image.At(site.rva, site.expected.size());
        if (!callRange ||
            std::memcmp(callRange->data(), site.expected.data(),
                        site.expected.size()) != 0) {
            return false;
        }
        std::byte* call = const_cast<std::byte*>(callRange->data());
        const auto replacement = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(call),
            reinterpret_cast<std::uintptr_t>(&FrameSignalWithPacingHook));
        if (!replacement ||
            !transaction->Add(
                site.rva - firstRva,
                std::as_bytes(std::span{site.expected}), *replacement)) {
            return false;
        }
    }

    g_originalFrameSignal = reinterpret_cast<FrameSignalFn>(frameSignal);
    if (transaction->Apply() != patch::ApplyStatus::Applied) {
        g_originalFrameSignal = nullptr;
        return false;
    }
    g_frameSignalCallsPatch = std::move(transaction);
    return true;
}

void RollBackInstallation() {
    frame_pacing::state::gameBoundaryPacingActive.store(
        false, std::memory_order_release);
    g_veffUpdateDetour.RevertAndClear(g_originalVeffUpdate);
    g_movieOpenDetour.RevertAndClear(g_originalMovieOpen);
    g_waitPatch.reset();
    if (g_frameSignalCallsPatch) {
        g_frameSignalCallsPatch->Revert();
        g_frameSignalCallsPatch.reset();
        g_originalFrameSignal = nullptr;
    }
    if (g_deltaCallsPatch) {
        g_deltaCallsPatch->Revert();
        g_deltaCallsPatch.reset();
        g_originalReadDelta = nullptr;
    }
    g_setterDetour.RevertAndClear(g_originalSetter);
}

BOOL CALLBACK InitializeImpl(PINIT_ONCE, PVOID, PVOID*) {
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (CurrentProfile().title != Title::FinalFantasyXIII2 || !image ||
        !Supports(Capability::FramePacing)) {
        LogWarning("Frame pacing fix disabled: unsupported executable or no title contract");
        return TRUE;
    }

    const auto setter = image->At(kSetterRva, kSetterPrefix.size());
    const auto statePointer = image->At(kStatePointerRva, sizeof(float*));
    const auto delta = image->At(kDeltaRva, kDeltaPrefix.size());
    const auto frameSignal = image->At(
        kFrameSignalRva, kFrameSignalPrefix.size());
    const auto wait = image->At(kWaitTimeoutRva, kWaitInfinite.size());
    const auto gameMainPointer = image->At(
        kGameMainPointerRva, sizeof(void*));
    const auto gameMainVtable = image->At(
        kGameMainVtableRva, sizeof(void*));
    const auto frameSourceVtable = image->At(
        kFrameSourceVtableRva, sizeof(void*));
    const auto movieOpen = image->At(kMovieOpenRva, kMovieOpenPrefix.size());
    const auto veffUpdate = image->At(kVeffUpdateRva, kVeffUpdatePrefix.size());
    if (!setter || !statePointer || !delta || !frameSignal || !wait ||
        !gameMainPointer || !gameMainVtable || !frameSourceVtable ||
        !movieOpen || !veffUpdate ||
        std::memcmp(setter->data(), kSetterPrefix.data(),
                    kSetterPrefix.size()) != 0 ||
        std::memcmp(delta->data(), kDeltaPrefix.data(),
                    kDeltaPrefix.size()) != 0 ||
        std::memcmp(frameSignal->data(), kFrameSignalPrefix.data(),
                    kFrameSignalPrefix.size()) != 0 ||
        std::memcmp(wait->data(), kWaitInfinite.data(),
                    kWaitInfinite.size()) != 0 ||
        std::memcmp(movieOpen->data(), kMovieOpenPrefix.data(),
                    kMovieOpenPrefix.size()) != 0 ||
        std::memcmp(veffUpdate->data(), kVeffUpdatePrefix.data(),
                    kVeffUpdatePrefix.size()) != 0) {
        LogWarning("Frame pacing fix disabled: XIII-2 executable contract mismatch");
        return TRUE;
    }

    bool installed = g_setterDetour.Install(
        "ff13-2-frame-limit-policy",
        const_cast<std::byte*>(setter->data()),
        reinterpret_cast<void*>(&SetFrameRateHook),
        std::as_bytes(std::span{kSetterPrefix}),
        reinterpret_cast<void**>(&g_originalSetter));
    if (installed) {
        installed = InstallDeltaCallRedirects(
            *image, const_cast<std::byte*>(delta->data()));
    }
    if (installed) {
        installed = InstallFrameSignalCallRedirects(
            *image, const_cast<std::byte*>(frameSignal->data()));
    }
    if (installed) {
        g_waitPatch = std::make_unique<patch::Transaction>(
            "ff13-2-high-refresh-render-wait",
            std::span<std::byte>(const_cast<std::byte*>(wait->data()),
                                 kWaitInfinite.size()));
        installed = g_waitPatch->Add(
            0u, std::as_bytes(std::span{kWaitInfinite}),
            std::as_bytes(std::span{kWaitYield}));
    }
    if (installed) {
        installed = g_movieOpenDetour.Install(
            "ff13-2-movie-open-preparation",
            const_cast<std::byte*>(movieOpen->data()),
            reinterpret_cast<void*>(&MovieOpenHook),
            std::as_bytes(std::span{kMovieOpenPrefix}),
            reinterpret_cast<void**>(&g_originalMovieOpen));
    }
    if (installed) {
        installed = g_veffUpdateDetour.Install(
            "ff13-2-menu-highlight-timing",
            const_cast<std::byte*>(veffUpdate->data()),
            reinterpret_cast<void*>(&VeffUpdateHook),
            std::as_bytes(std::span{kVeffUpdatePrefix}),
            reinterpret_cast<void**>(&g_originalVeffUpdate));
    }
    if (!installed) {
        RollBackInstallation();
        LogWarning("Frame pacing fix disabled: XIII-2 setter ownership failed");
        return TRUE;
    }

    g_frameStatePointer = reinterpret_cast<float**>(
        const_cast<std::byte*>(statePointer->data()));
    g_gameMainPointer = reinterpret_cast<void**>(
        const_cast<std::byte*>(gameMainPointer->data()));
    g_gameMainVtable = const_cast<std::byte*>(gameMainVtable->data());
    g_frameSourceVtable = const_cast<std::byte*>(
        frameSourceVtable->data());
    QueryPerformanceFrequency(&g_counterFrequency);
    g_supported.store(true, std::memory_order_release);
    return TRUE;
}

bool Activate() {
    if (!g_supported.load(std::memory_order_acquire) ||
        !g_frameStatePointer) {
        return false;
    }
    float* state = *g_frameStatePointer;
    if (!patch::memory::IsWritable(state, sizeof(float) * 2u)) return false;

    g_frameState = state;
    frame_pacing::state::ownershipActive.store(
        true, std::memory_order_release);
    g_deltaGeneration.fetch_add(1u, std::memory_order_release);
    return true;
}

void SetGameBoundaryPacingActive(bool active, unsigned frameRateLimit) {
    const bool previous =
        frame_pacing::state::gameBoundaryPacingActive.exchange(
            active, std::memory_order_acq_rel);
    if (previous == active) return;
    if (active) {
        if (!g_loggedGameBoundaryPacingActive.exchange(
                true, std::memory_order_acq_rel)) {
            if (frameRateLimit == 60u) {
                Log("XIII-2 game-boundary cadence=60000/1001 Hz");
            } else {
                Log("XIII-2 game-boundary cadence=%u Hz",
                        frameRateLimit);
            }
        }
        return;
    }
    Log("XIII-2 Store-style game-boundary pacing inactive: presentation fallback");
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(
        &g_initializeOnce, &InitializeImpl, nullptr, nullptr);
}

void Pump() {
    if (!g_supported.load(std::memory_order_acquire)) return;

    const settings::PerformanceSettings settings =
        settings::ActivePerformance();
    AcquireSRWLockExclusive(&g_stateLock);
    bool highRefreshActive = false;
    // FrameRateLimit changes only our wait; native hooks stay installed for process life
    const bool supportedProfile = settings.frameRateLimit == 0u ||
                                  settings.frameRateLimit >= 30u;
    if (settings.highFpsCorrectness && supportedProfile) {
        bool presentationMode = false;
        ApplyFrameSourcePolicy(settings, presentationMode);
        highRefreshActive = presentationMode &&
            (settings.frameRateLimit == 0u ||
             settings.frameRateLimit > 30u) &&
            !IsMovieOpenPending();
    } else {
        RestoreFrameSource();
    }
    if (g_waitPatch) {
        if (highRefreshActive) {
            if (!g_waitPatch->IsApplied()) {
                const ULONGLONG now = GetTickCount64();
                if (now < g_waitPatchRetryAt) {
                    highRefreshActive = false;
                } else {
                    const patch::ApplyStatus status = g_waitPatch->Apply();
                    if (status != patch::ApplyStatus::Applied &&
                        status != patch::ApplyStatus::AlreadyApplied) {
                        if (!g_waitPatchApplyFailureLogged) {
                            LogError("XIII-2 render-wait bypass rejected status=%s; retry-interval=%llums",
                                patch::ApplyStatusName(status),
                                static_cast<unsigned long long>(
                                    kWaitPatchRetryIntervalMs));
                            g_waitPatchApplyFailureLogged = true;
                        }
                        g_waitPatchRetryAt =
                            now + kWaitPatchRetryIntervalMs;
                        highRefreshActive = false;
                    } else {
                        g_waitPatchRetryAt = 0u;
                        g_waitPatchApplyFailureLogged = false;
                        g_waitPatchRevertFailureLogged = false;
                    }
                }
            } else {
                g_waitPatchRetryAt = 0u;
                g_waitPatchApplyFailureLogged = false;
                g_waitPatchRevertFailureLogged = false;
            }
        } else if (g_waitPatch->IsApplied()) {
            const ULONGLONG now = GetTickCount64();
            if (now >= g_waitPatchRetryAt) {
                if (!g_waitPatch->Revert()) {
                    if (!g_waitPatchRevertFailureLogged) {
                        LogError("XIII-2 render-wait bypass could not be restored for native mode; retry-interval=%llums",
                            static_cast<unsigned long long>(
                                kWaitPatchRetryIntervalMs));
                        g_waitPatchRevertFailureLogged = true;
                    }
                    g_waitPatchRetryAt =
                        now + kWaitPatchRetryIntervalMs;
                } else {
                    g_waitPatchRetryAt = 0u;
                    g_waitPatchApplyFailureLogged = false;
                    g_waitPatchRevertFailureLogged = false;
                }
            }
        } else {
            g_waitPatchRetryAt = 0u;
            g_waitPatchApplyFailureLogged = false;
            g_waitPatchRevertFailureLogged = false;
        }
    }

    const bool wasHighRefresh =
        frame_pacing::state::highRefreshPresentationActive.load(
            std::memory_order_relaxed);
    if (wasHighRefresh != highRefreshActive) {
        frame_pacing::state::highRefreshPresentationActive.store(
            highRefreshActive, std::memory_order_release);
        g_deltaGeneration.fetch_add(1u, std::memory_order_release);
        if (!highRefreshActive) {
            frame_pacing::state::appliedPresentationDeltaTicks.store(
                0u, std::memory_order_release);
        }
        Log("XIII-2 decoupled presentation %s: authored=%.5f Hz presentation=%s",
            highRefreshActive ? "active" : "inactive",
            kAuthoredSimulationRate,
            settings.frameRateLimit == 0u ? "unlimited" : "high-refresh");
    }

    if (!frame_pacing::state::ownershipActive.load(
            std::memory_order_acquire)) {
        const bool activated = Activate();
        SetGameBoundaryPacingActive(
            activated && highRefreshActive &&
                settings.frameRateLimit != 0u,
            settings.frameRateLimit);
        ReleaseSRWLockExclusive(&g_stateLock);
        return;
    }

    float* replacement = g_frameStatePointer
        ? *g_frameStatePointer : nullptr;
    if (replacement != g_frameState) {
        if (!patch::memory::IsWritable(
                replacement, sizeof(float) * 2u)) {
            g_frameState = nullptr;
            frame_pacing::state::ownershipActive.store(
                false, std::memory_order_release);
            SetGameBoundaryPacingActive(false, settings.frameRateLimit);
            Log("Smooth frame pacing is waiting for a recreated game timing object");
            ReleaseSRWLockExclusive(&g_stateLock);
            return;
        }
        g_frameState = replacement;
        Log("Smooth frame pacing rebound to a recreated game timing object");
    }

    SetGameBoundaryPacingActive(
        highRefreshActive && settings.frameRateLimit != 0u,
        settings.frameRateLimit);

    ReleaseSRWLockExclusive(&g_stateLock);
}

} // namespace novafix::game::xiii2_frame_pacing_runtime
