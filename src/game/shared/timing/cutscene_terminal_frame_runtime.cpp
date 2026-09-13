#include "game/shared/timing/cutscene_terminal_frame_runtime.h"

#include "game/shared/timing/cutscene_terminal_frame.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "performance/frame_metrics.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace novafix::game::cutscene_terminal_frame::runtime {
namespace {

constexpr std::size_t kSchedulerPrimaryListOffset = 0x3Cu;
constexpr std::size_t kSchedulerSecondaryListOffset = 0x48u;
constexpr std::size_t kMaximumSchedulerListNodes = 256u;
constexpr std::size_t kMaximumTrackedBlends = 64u;
constexpr std::uint32_t kMinimumShotSchedulerClipCount = 4u;
constexpr ULONGLONG kMaximumTerminalFrameHoldMilliseconds = 45u;

using ClipCallbackFn = std::uintptr_t(__thiscall*)(void*, void*);
using SchedulerCleanupFn = std::uintptr_t(__thiscall*)(void*);
using SchedulerEnterFn = std::uintptr_t(__thiscall*)(void*, int);

patch::Detour32 g_crossFadeEnterDetour;
patch::Detour32 g_clipEndDetour;
patch::Detour32 g_schedulerCleanupDetour;
patch::Detour32 g_schedulerEnterDetour;
patch::Detour32 g_commonClipEnterDetour;
patch::Detour32 g_screenEnvBlendEnterDetour;
patch::Detour32 g_whiteFadeEnterDetour;
patch::Detour32 g_whiteAllClipFadeEnterDetour;
patch::Detour32 g_fadeScreenEnterDetour;
patch::Detour32 g_captureEnterDetour;
ClipCallbackFn g_originalCrossFadeEnter{};
ClipCallbackFn g_originalClipEnd{};
SchedulerCleanupFn g_originalSchedulerCleanup{};
SchedulerEnterFn g_originalSchedulerEnter{};
ClipCallbackFn g_originalCommonClipEnter{};
ClipCallbackFn g_originalScreenEnvBlendEnter{};
ClipCallbackFn g_originalWhiteFadeEnter{};
ClipCallbackFn g_originalWhiteAllClipFadeEnter{};
ClipCallbackFn g_originalFadeScreenEnter{};
ClipCallbackFn g_originalCaptureEnter{};
std::uintptr_t g_crossFadeVtable{};
std::uintptr_t g_imageBase{};
Contract g_contract;
const std::int32_t* g_liveReferenceStep{};
std::array<void*, kMaximumTrackedBlends> g_crossFades{};
std::array<void*, kMaximumTrackedBlends> g_authoredVisualBlends{};
SRWLOCK g_crossFadeLock = SRWLOCK_INIT;
SRWLOCK g_authoredVisualBlendLock = SRWLOCK_INIT;
std::atomic_uintptr_t g_holdScheduler{};
std::atomic_uint64_t g_holdDeadlineMilliseconds{};
std::atomic_bool g_holdCameraEnded{};
std::atomic_uintptr_t g_cinematicScheduler{};
thread_local void* g_currentCleanupScheduler{};
thread_local void* g_currentEnterScheduler{};

template <typename T>
bool ReadField(const void* object, std::size_t offset, T& value) {
    if (!object) return false;
    const auto* address = static_cast<const std::byte*>(object) + offset;
    if (!patch::memory::IsReadable(address, sizeof(T))) return false;
    std::memcpy(static_cast<void*>(&value), address, sizeof(T));
    return true;
}

template <typename T>
T ReadTrustedField(const void* object, std::size_t offset) {
    T value{};
    if (object) {
        std::memcpy(
            &value, static_cast<const std::byte*>(object) + offset,
            sizeof(value));
    }
    return value;
}

struct SchedulerListSummary {
    std::uint32_t count{};
    bool complete{true};
};

struct SchedulerSummary {
    SchedulerListSummary primary;
    SchedulerListSummary secondary;
    bool readable{};
};

SchedulerListSummary SummarizeSchedulerList(
    const void* scheduler, std::size_t listOffset) {
    SchedulerListSummary summary;
    void* node{};
    if (!ReadField(scheduler, listOffset, node)) {
        summary.complete = false;
        return summary;
    }
    for (std::size_t index = 0u; node; ++index) {
        if (index >= kMaximumSchedulerListNodes) {
            summary.complete = false;
            break;
        }
        void* next{};
        if (!ReadField(node, 4u, next)) {
            summary.complete = false;
            break;
        }
        ++summary.count;
        if (next == node) {
            summary.complete = false;
            break;
        }
        node = next;
    }
    return summary;
}

SchedulerSummary SummarizeScheduler(const void* scheduler) {
    SchedulerSummary summary;
    std::uint32_t cursor{};
    if (!ReadField(scheduler, g_contract.schedulerCursorOffset, cursor)) {
        return summary;
    }
    summary.primary = SummarizeSchedulerList(
        scheduler, kSchedulerPrimaryListOffset);
    summary.secondary = SummarizeSchedulerList(
        scheduler, kSchedulerSecondaryListOffset);
    summary.readable = summary.primary.complete && summary.secondary.complete;
    return summary;
}

bool SchedulerListContainsVtable(const void* scheduler,
                                 std::size_t listOffset,
                                 std::uintptr_t targetVtable) {
    void* node{};
    if (!targetVtable || !ReadField(scheduler, listOffset, node)) return false;
    for (std::size_t index = 0u; node && index < kMaximumSchedulerListNodes;
         ++index) {
        void* next{};
        void* vtable{};
        const auto* clip = static_cast<const std::byte*>(node) - 8u;
        if (!ReadField(node, 4u, next) || !ReadField(clip, 0u, vtable)) {
            return false;
        }
        if (reinterpret_cast<std::uintptr_t>(vtable) == targetVtable) {
            return true;
        }
        if (next == node) return false;
        node = next;
    }
    return false;
}

bool SchedulerContainsCameraClip(const void* scheduler) {
    for (const std::uint32_t rva : g_contract.cameraClipVtableRvas) {
        if (!rva) continue;
        const std::uintptr_t cameraVtable = g_imageBase + rva;
        if (SchedulerListContainsVtable(
                scheduler, kSchedulerPrimaryListOffset, cameraVtable) ||
            SchedulerListContainsVtable(
                scheduler, kSchedulerSecondaryListOffset, cameraVtable)) {
            return true;
        }
    }
    return false;
}

template <std::size_t Size>
void SetTracked(std::array<void*, Size>& entries, SRWLOCK& lock,
                void* clip, bool active) {
    AcquireSRWLockExclusive(&lock);
    if (active) {
        bool present = false;
        for (void* value : entries) {
            if (value == clip) present = true;
        }
        if (!present) {
            for (void*& value : entries) {
                if (!value) {
                    value = clip;
                    break;
                }
            }
        }
    } else {
        for (void*& value : entries) {
            if (value == clip) value = nullptr;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}

template <std::size_t Size>
std::uint32_t TrackedCount(const std::array<void*, Size>& entries,
                           SRWLOCK& lock) {
    std::uint32_t count{};
    AcquireSRWLockShared(&lock);
    for (void* value : entries) {
        if (value) ++count;
    }
    ReleaseSRWLockShared(&lock);
    return count;
}

std::uint32_t ActiveAuthoredBlendCount() {
    return TrackedCount(g_crossFades, g_crossFadeLock) +
        TrackedCount(g_authoredVisualBlends, g_authoredVisualBlendLock);
}

bool IsAuthoredVisualBlend(std::uintptr_t vtableRva) {
    for (const std::uint32_t candidate :
         g_contract.authoredBlendVtableRvas) {
        if (candidate != 0u && candidate == vtableRva) return true;
    }
    return false;
}

bool ContainsVtableRva(std::span<const std::uint32_t> candidates,
                       std::uintptr_t vtableRva) {
    for (const std::uint32_t candidate : candidates) {
        if (candidate && candidate == vtableRva) return true;
    }
    return false;
}

bool IsCameraClip(std::uintptr_t vtableRva) {
    return ContainsVtableRva(g_contract.cameraClipVtableRvas, vtableRva);
}

bool IsMotionClip(std::uintptr_t vtableRva) {
    return ContainsVtableRva(g_contract.motionClipVtableRvas, vtableRva);
}

std::uintptr_t VtableRva(const void* clip) {
    void* vtable{};
    if (!ReadField(clip, 0u, vtable)) return 0u;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(vtable);
    return g_imageBase && address >= g_imageBase ? address - g_imageBase : 0u;
}

bool IsCrossFadeClip(const void* clip) {
    void* vtable{};
    return clip && g_crossFadeVtable != 0u &&
        ReadField(clip, 0u, vtable) &&
        reinterpret_cast<std::uintptr_t>(vtable) == g_crossFadeVtable;
}

void ResetHoldState() {
    g_holdDeadlineMilliseconds.store(0u, std::memory_order_release);
    g_holdCameraEnded.store(false, std::memory_order_release);
}

void ClearAnyHold() {
    if (g_holdScheduler.exchange(0u, std::memory_order_acq_rel)) {
        ResetHoldState();
    }
}

void ClearHoldForScheduler(void* scheduler) {
    std::uintptr_t expected = reinterpret_cast<std::uintptr_t>(scheduler);
    if (expected && g_holdScheduler.compare_exchange_strong(
            expected, 0u, std::memory_order_acq_rel)) {
        ResetHoldState();
    }
}

void ArmTerminalFrameHold(void* scheduler) {
    if (!scheduler || ActiveAuthoredBlendCount() != 0u) return;
    const SchedulerSummary summary = SummarizeScheduler(scheduler);
    if (!summary.readable ||
        summary.primary.count < kMinimumShotSchedulerClipCount ||
        !SchedulerContainsCameraClip(scheduler)) {
        return;
    }

    const std::uintptr_t candidate = reinterpret_cast<std::uintptr_t>(scheduler);
    std::uintptr_t expected = 0u;
    const bool newlyArmed = g_holdScheduler.compare_exchange_strong(
        expected, candidate, std::memory_order_acq_rel);
    if (newlyArmed || expected == candidate) {
        if (newlyArmed) {
            g_holdCameraEnded.store(false, std::memory_order_release);
        }
        g_holdDeadlineMilliseconds.store(
            GetTickCount64() + kMaximumTerminalFrameHoldMilliseconds,
            std::memory_order_release);
    }
}

void ReleaseAfterNextPresent(void* scheduler) {
    if (g_holdScheduler.load(std::memory_order_acquire) ==
        reinterpret_cast<std::uintptr_t>(scheduler)) {
        g_holdCameraEnded.store(true, std::memory_order_release);
    }
}

EndpointSample SampleEndpoint(
    const void* clip, std::uintptr_t vtableRva,
    std::int32_t referenceStep) {
    EndpointSample sample;
    void* data = ReadTrustedField<void*>(clip, 0x14u);
    const std::int32_t state =
        ReadTrustedField<std::int32_t>(clip, 0x1Cu);
    const std::int32_t current =
        ReadTrustedField<std::int32_t>(clip, 0x28u);
    const std::int32_t step =
        ReadTrustedField<std::int32_t>(clip, 0x2Cu);
    if (!data || state != 2 || step <= 0) return sample;

    const std::int32_t start =
        ReadTrustedField<std::int32_t>(data, 0x04u);
    std::int32_t end{};
    if (IsCameraClip(vtableRva)) {
        end = ReadTrustedField<std::int32_t>(data, 0x10u);
    } else if (IsMotionClip(vtableRva)) {
        end = ReadTrustedField<std::int32_t>(data, 0x18u);
        end = g_contract.endpointPolicy.adjustMotionEnd(start, end);
    } else {
        return sample;
    }

    const std::int64_t remaining =
        static_cast<std::int64_t>(end) - start - current;
    if (!g_contract.endpointPolicy.endpointInLookahead(
            remaining, referenceStep)) {
        return sample;
    }
    sample.remaining = static_cast<std::int32_t>(remaining);
    sample.step = step;
    sample.approaching = true;
    return sample;
}

void KeepClosestEndpoint(EndpointSample sample, EndpointSample& closest) {
    if (sample.approaching &&
        (!closest.approaching || sample.remaining < closest.remaining)) {
        closest = sample;
    }
}

void ScanEndpointList(
    const void* scheduler, std::size_t listOffset,
    std::int32_t referenceStep,
    EndpointSample& camera, EndpointSample& motion) {
    void* node = ReadTrustedField<void*>(scheduler, listOffset);
    for (std::size_t index = 0u;
         node && index < kMaximumSchedulerListNodes; ++index) {
        void* next = ReadTrustedField<void*>(node, 4u);
        const auto* clip = static_cast<const std::byte*>(node) - 8u;
        void* vtable = ReadTrustedField<void*>(clip, 0u);
        const std::uintptr_t address =
            reinterpret_cast<std::uintptr_t>(vtable);
        const std::uintptr_t vtableRva =
            g_imageBase && address >= g_imageBase
                ? address - g_imageBase : 0u;
        const EndpointSample sample =
            SampleEndpoint(clip, vtableRva, referenceStep);
        if (IsCameraClip(vtableRva)) KeepClosestEndpoint(sample, camera);
        if (IsMotionClip(vtableRva)) KeepClosestEndpoint(sample, motion);
        if (next == node) return;
        node = next;
    }
}

std::int32_t ReferenceStep() {
    if (g_liveReferenceStep) {
        return ReadTrustedField<std::int32_t>(g_liveReferenceStep, 0u);
    }
    return g_contract.fixedReferenceStep;
}

void ArmUpcomingTerminalCut(void* scheduler) {
    if (!scheduler || ActiveAuthoredBlendCount() != 0u ||
        g_cinematicScheduler.load(std::memory_order_acquire) !=
            reinterpret_cast<std::uintptr_t>(scheduler)) {
        return;
    }
    const float frameMilliseconds = performance::CurrentFrameMilliseconds();
    const std::int32_t referenceStep = ReferenceStep();
    if (!g_contract.endpointPolicy.highRefresh(
            frameMilliseconds, referenceStep)) {
        return;
    }

    EndpointSample camera;
    EndpointSample motion;
    ScanEndpointList(scheduler, kSchedulerPrimaryListOffset,
                     referenceStep, camera, motion);
    ScanEndpointList(scheduler, kSchedulerSecondaryListOffset,
                     referenceStep, camera, motion);
    if (!camera.approaching || !motion.approaching) return;

    if (g_contract.endpointPolicy.crossesTerminal(
            camera, motion, referenceStep)) {
        ArmTerminalFrameHold(scheduler);
    }
}

std::uintptr_t __fastcall CommonClipEnterHook(
    void* clip, void*, void* context) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.common-clip-enter-total");
    if (!g_originalCommonClipEnter) return 0u;
    if (g_currentEnterScheduler &&
        IsCameraClip(VtableRva(clip))) {
        g_cinematicScheduler.store(
            reinterpret_cast<std::uintptr_t>(g_currentEnterScheduler),
            std::memory_order_release);
    }
    return g_originalCommonClipEnter(clip, context);
}

std::uintptr_t __fastcall SchedulerEnterHook(
    void* scheduler, void*, int cutTime) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.scheduler-enter-total");
    if (!g_originalSchedulerEnter) return 0u;
    void* previous = g_currentEnterScheduler;
    g_currentEnterScheduler = scheduler;
    const std::uintptr_t result =
        g_originalSchedulerEnter(scheduler, cutTime);
    g_currentEnterScheduler = previous;
    return result;
}

std::uintptr_t __fastcall CrossFadeEnterHook(
    void* clip, void*, void* context) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.cross-fade-enter-total");
    if (!g_originalCrossFadeEnter) return 0u;
    const std::uintptr_t result = g_originalCrossFadeEnter(clip, context);
    SetTracked(g_crossFades, g_crossFadeLock, clip, true);
    ClearAnyHold();
    return result;
}

std::uintptr_t __fastcall ScreenEnvBlendEnterHook(
    void* clip, void*, void* context) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.visual-blend-enter-total");
    if (!g_originalScreenEnvBlendEnter) return 0u;
    const bool authored = !g_contract.screenEnvBlendChecksVtable ||
        IsAuthoredVisualBlend(VtableRva(clip));
    const std::uintptr_t result =
        g_originalScreenEnvBlendEnter(clip, context);
    if (authored) {
        SetTracked(
            g_authoredVisualBlends, g_authoredVisualBlendLock, clip, true);
        ClearAnyHold();
    }
    return result;
}

std::uintptr_t EnterTrackedBlend(ClipCallbackFn original,
                                 void* clip, void* context) {
    if (!original) return 0u;
    const std::uintptr_t result = original(clip, context);
    SetTracked(g_authoredVisualBlends, g_authoredVisualBlendLock, clip, true);
    ClearAnyHold();
    return result;
}

std::uintptr_t __fastcall WhiteFadeEnterHook(
    void* clip, void*, void* context) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.white-fade-enter-total");
    return EnterTrackedBlend(g_originalWhiteFadeEnter, clip, context);
}

std::uintptr_t __fastcall WhiteAllClipFadeEnterHook(
    void* clip, void*, void* context) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.all-fade-enter-total");
    return EnterTrackedBlend(g_originalWhiteAllClipFadeEnter, clip, context);
}

std::uintptr_t __fastcall FadeScreenEnterHook(
    void* clip, void*, void* context) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.fade-screen-enter-total");
    return EnterTrackedBlend(g_originalFadeScreenEnter, clip, context);
}

std::uintptr_t __fastcall CaptureEnterHook(
    void* clip, void*, void* context) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.capture-enter-total");
    return EnterTrackedBlend(g_originalCaptureEnter, clip, context);
}

std::uintptr_t __fastcall ClipEndHook(
    void* clip, void*, void* context) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.clip-end-total");
    if (!g_originalClipEnd) return 0u;
    const std::uintptr_t vtableRva = VtableRva(clip);
    const bool crossFade = IsCrossFadeClip(clip);
    const bool authoredVisualBlend = IsAuthoredVisualBlend(vtableRva);
    if (g_contract.effectClipFallback && g_currentCleanupScheduler &&
        vtableRva == g_contract.effectClipVtableRva) {
        ArmTerminalFrameHold(g_currentCleanupScheduler);
    } else if (g_currentCleanupScheduler &&
               IsCameraClip(vtableRva)) {
        ReleaseAfterNextPresent(g_currentCleanupScheduler);
    }

    const std::uintptr_t result = g_originalClipEnd(clip, context);
    if (authoredVisualBlend) {
        SetTracked(
            g_authoredVisualBlends, g_authoredVisualBlendLock, clip, false);
    }
    if (crossFade) {
        SetTracked(g_crossFades, g_crossFadeLock, clip, false);
    }
    return result;
}

std::uintptr_t __fastcall SchedulerCleanupHook(void* scheduler, void*) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.cutscene.scheduler-cleanup-total");
    if (!g_originalSchedulerCleanup) return 0u;
    if (g_cinematicScheduler.load(std::memory_order_acquire) !=
        reinterpret_cast<std::uintptr_t>(scheduler)) {
        return g_originalSchedulerCleanup(scheduler);
    }
    ArmUpcomingTerminalCut(scheduler);
    void* previous = g_currentCleanupScheduler;
    g_currentCleanupScheduler = scheduler;
    const std::uintptr_t result = g_originalSchedulerCleanup(scheduler);
    g_currentCleanupScheduler = previous;
    return result;
}

void RevertCoreHooks() {
    bool clean = true;
    clean = g_commonClipEnterDetour.RevertAndClear(
                g_originalCommonClipEnter) && clean;
    clean = g_schedulerEnterDetour.RevertAndClear(
                g_originalSchedulerEnter) && clean;
    clean = g_schedulerCleanupDetour.RevertAndClear(
                g_originalSchedulerCleanup) && clean;
    clean = g_clipEndDetour.RevertAndClear(g_originalClipEnd) && clean;
    clean = g_crossFadeEnterDetour.RevertAndClear(
                g_originalCrossFadeEnter) && clean;
    if (clean) g_crossFadeVtable = 0u;
}

bool InstallCoreHooks(const ImageView& image) {
    const auto crossFade = image.At(
        g_contract.crossFadeEnter.rva,
        g_contract.crossFadeEnter.expected.size());
    const auto end = image.At(
        g_contract.clipEnd.rva, g_contract.clipEnd.expected.size());
    const auto cleanup = image.At(
        g_contract.schedulerCleanup.rva,
        g_contract.schedulerCleanup.expected.size());
    const auto crossFadeVtable = image.Address32(
        g_contract.crossFadeVtableRva, sizeof(void*));
    if (!crossFade || !end || !cleanup || !crossFadeVtable) return false;

    g_crossFadeVtable = *crossFadeVtable;
    const bool crossFadeInstalled = g_crossFadeEnterDetour.Install(
        "cutscene-terminal-frame-crossfade-fix",
        const_cast<std::byte*>(crossFade->data()),
        reinterpret_cast<void*>(&CrossFadeEnterHook),
        g_contract.crossFadeEnter.expected,
        reinterpret_cast<void**>(&g_originalCrossFadeEnter));
    const bool endInstalled = crossFadeInstalled && g_clipEndDetour.Install(
        "cutscene-terminal-frame-end-fix",
        const_cast<std::byte*>(end->data()),
        reinterpret_cast<void*>(&ClipEndHook),
        g_contract.clipEnd.expected,
        reinterpret_cast<void**>(&g_originalClipEnd));
    const bool cleanupInstalled = endInstalled &&
        g_schedulerCleanupDetour.Install(
            "cutscene-terminal-frame-cleanup-fix",
            const_cast<std::byte*>(cleanup->data()),
            reinterpret_cast<void*>(&SchedulerCleanupHook),
            g_contract.schedulerCleanup.expected,
            reinterpret_cast<void**>(&g_originalSchedulerCleanup));
    if (!cleanupInstalled) {
        RevertCoreHooks();
        return false;
    }
    return true;
}

bool InstallEndpointHooks(const ImageView& image) {
    const auto enter = image.At(
        g_contract.schedulerEnter.rva,
        g_contract.schedulerEnter.expected.size());
    const auto commonEnter = image.At(
        g_contract.commonClipEnter.rva,
        g_contract.commonClipEnter.expected.size());
    if (!enter || !commonEnter) return false;

    const bool enterInstalled = g_schedulerEnterDetour.Install(
        "cutscene-terminal-frame-enter-fix",
        const_cast<std::byte*>(enter->data()),
        reinterpret_cast<void*>(&SchedulerEnterHook),
        g_contract.schedulerEnter.expected,
        reinterpret_cast<void**>(&g_originalSchedulerEnter));
    const bool commonEnterInstalled = enterInstalled &&
        g_commonClipEnterDetour.Install(
            "cutscene-terminal-frame-camera-identification",
            const_cast<std::byte*>(commonEnter->data()),
            reinterpret_cast<void*>(&CommonClipEnterHook),
            g_contract.commonClipEnter.expected,
            reinterpret_cast<void**>(&g_originalCommonClipEnter));
    if (!commonEnterInstalled) {
        g_commonClipEnterDetour.RevertAndClear(g_originalCommonClipEnter);
        g_schedulerEnterDetour.RevertAndClear(g_originalSchedulerEnter);
        return false;
    }
    return true;
}

void RevertBlendHooks() {
    g_captureEnterDetour.RevertAndClear(g_originalCaptureEnter);
    g_fadeScreenEnterDetour.RevertAndClear(g_originalFadeScreenEnter);
    g_whiteAllClipFadeEnterDetour.RevertAndClear(
        g_originalWhiteAllClipFadeEnter);
    g_whiteFadeEnterDetour.RevertAndClear(g_originalWhiteFadeEnter);
    g_screenEnvBlendEnterDetour.RevertAndClear(
        g_originalScreenEnvBlendEnter);
}

bool ValidateRuntimeVtables(const ImageView& image) {
    if (!image.Address32(g_contract.effectClipVtableRva, sizeof(void*))) {
        return false;
    }
    for (const std::uint32_t rva : g_contract.cameraClipVtableRvas) {
        if (rva && !image.Address32(rva, sizeof(void*))) return false;
    }
    for (const std::uint32_t rva : g_contract.motionClipVtableRvas) {
        if (rva && !image.Address32(rva, sizeof(void*))) return false;
    }
    for (const std::uint32_t rva : g_contract.authoredBlendVtableRvas) {
        if (!image.Address32(rva, sizeof(void*))) return false;
    }
    return true;
}

bool InstallBlendHooks(const ImageView& image) {
    const auto screenEnv = image.At(
        g_contract.screenEnvBlendEnter.rva,
        g_contract.screenEnvBlendEnter.expected.size());
    const auto whiteFade = image.At(
        g_contract.whiteFadeEnter.rva,
        g_contract.whiteFadeEnter.expected.size());
    const auto whiteAll = image.At(
        g_contract.whiteAllClipFadeEnter.rva,
        g_contract.whiteAllClipFadeEnter.expected.size());
    const auto fadeScreen = image.At(
        g_contract.fadeScreenEnter.rva,
        g_contract.fadeScreenEnter.expected.size());
    if (!screenEnv || !whiteFade || !whiteAll || !fadeScreen) {
        return false;
    }

    std::vector<std::byte> whiteFadeExpected(
        g_contract.whiteFadeEnter.expected.begin(),
        g_contract.whiteFadeEnter.expected.end());
    if (g_contract.whiteFadeAbsoluteTargetRva != 0u) {
        const std::size_t operand =
            g_contract.whiteFadeAbsoluteOperandOffset;
        if (operand + sizeof(std::uint32_t) > whiteFadeExpected.size()) {
            return false;
        }
        const auto absoluteTarget = image.Address32(
            g_contract.whiteFadeAbsoluteTargetRva);
        if (!absoluteTarget) return false;
        std::memcpy(
            whiteFadeExpected.data() + operand, &*absoluteTarget,
            sizeof(*absoluteTarget));
    }

    const bool screenEnvInstalled = g_screenEnvBlendEnterDetour.Install(
        "cutscene-screen-env-blend-guard",
        const_cast<std::byte*>(screenEnv->data()),
        reinterpret_cast<void*>(&ScreenEnvBlendEnterHook),
        g_contract.screenEnvBlendEnter.expected,
        reinterpret_cast<void**>(&g_originalScreenEnvBlendEnter));
    const bool whiteFadeInstalled = screenEnvInstalled &&
        g_whiteFadeEnterDetour.Install(
            "cutscene-white-fade-guard",
            const_cast<std::byte*>(whiteFade->data()),
            reinterpret_cast<void*>(&WhiteFadeEnterHook),
            whiteFadeExpected,
            reinterpret_cast<void**>(&g_originalWhiteFadeEnter));
    const bool whiteAllInstalled = whiteFadeInstalled &&
        g_whiteAllClipFadeEnterDetour.Install(
            "cutscene-white-all-clip-fade-guard",
            const_cast<std::byte*>(whiteAll->data()),
            reinterpret_cast<void*>(&WhiteAllClipFadeEnterHook),
            g_contract.whiteAllClipFadeEnter.expected,
            reinterpret_cast<void**>(&g_originalWhiteAllClipFadeEnter));
    const bool fadeScreenInstalled = whiteAllInstalled &&
        g_fadeScreenEnterDetour.Install(
            "cutscene-fade-screen-guard",
            const_cast<std::byte*>(fadeScreen->data()),
            reinterpret_cast<void*>(&FadeScreenEnterHook),
            g_contract.fadeScreenEnter.expected,
            reinterpret_cast<void**>(&g_originalFadeScreenEnter));
    bool captureInstalled = fadeScreenInstalled;
    if (captureInstalled && g_contract.captureEnter) {
        const auto capture = image.At(
            g_contract.captureEnter.rva,
            g_contract.captureEnter.expected.size());
        captureInstalled = capture && g_captureEnterDetour.Install(
            "cutscene-capture-guard",
            const_cast<std::byte*>(capture->data()),
            reinterpret_cast<void*>(&CaptureEnterHook),
            g_contract.captureEnter.expected,
            reinterpret_cast<void**>(&g_originalCaptureEnter));
    }
    if (!captureInstalled) {
        RevertBlendHooks();
        return false;
    }
    return true;
}

bool ValidateContract(const Contract& contract) {
    return contract.crossFadeEnter && contract.clipEnd &&
        contract.schedulerCleanup &&
        contract.schedulerEnter && contract.commonClipEnter &&
        contract.crossFadeVtableRva != 0u &&
        contract.effectClipVtableRva != 0u &&
        !contract.cameraClipVtableRvas.empty() &&
        !contract.motionClipVtableRvas.empty() &&
        contract.schedulerCursorOffset != 0u &&
        !contract.authoredBlendVtableRvas.empty() &&
        contract.screenEnvBlendEnter && contract.whiteFadeEnter &&
        contract.whiteAllClipFadeEnter && contract.fadeScreenEnter &&
        (contract.fixedReferenceStep > 0 ||
         contract.liveReferenceStepRva != 0u) &&
        contract.endpointPolicy.adjustMotionEnd &&
        contract.endpointPolicy.endpointInLookahead &&
        contract.endpointPolicy.highRefresh &&
        contract.endpointPolicy.crossesTerminal;
}

} // namespace

bool Install(const Contract& contract) {
    if (!ValidateContract(contract)) {
        LogWarning("Cutscene terminal-frame fix disabled: invalid contract");
        return false;
    }

    const HMODULE executable = GetModuleHandleW(nullptr);
    const auto image = ImageView::FromModule(executable);
    if (!image) {
        LogWarning("Cutscene terminal-frame fix unavailable: main image missing");
        return false;
    }

    g_contract = contract;
    g_imageBase = reinterpret_cast<std::uintptr_t>(executable);
    if (g_contract.liveReferenceStepRva != 0u) {
        const auto referenceStep = image->Address32(
            g_contract.liveReferenceStepRva, sizeof(std::int32_t));
        if (!referenceStep) {
            LogWarning(
                "Cutscene terminal-frame fix unavailable: cut clock rejected");
            return false;
        }
        g_liveReferenceStep = reinterpret_cast<const std::int32_t*>(
            static_cast<std::uintptr_t>(*referenceStep));
    }
    if (!ValidateRuntimeVtables(*image)) {
        LogWarning(
            "Cutscene terminal-frame fix unavailable: vtable contract rejected");
        return false;
    }

    const bool coreInstalled = InstallCoreHooks(*image);
    const bool endpointInstalled =
        coreInstalled && InstallEndpointHooks(*image);
    const bool blendGuardsInstalled =
        endpointInstalled && InstallBlendHooks(*image);
    if (!blendGuardsInstalled) {
        if (coreInstalled) RevertCoreHooks();
        LogWarning(
            "Cutscene terminal-frame fix disabled: contract/install failure");
        return false;
    }

    return true;
}

bool ShouldSuppressPresent() {
    const ULONGLONG now = GetTickCount64();
    std::uintptr_t scheduler =
        g_holdScheduler.load(std::memory_order_acquire);
    if (!scheduler) return false;

    const ULONGLONG deadline =
        g_holdDeadlineMilliseconds.load(std::memory_order_acquire);
    if (!deadline || now > deadline) {
        if (g_holdScheduler.compare_exchange_strong(
                scheduler, 0u, std::memory_order_acq_rel)) {
            ResetHoldState();
        }
        return false;
    }
    if (ActiveAuthoredBlendCount() != 0u) {
        ClearAnyHold();
        return false;
    }

    if (g_holdCameraEnded.load(std::memory_order_acquire)) {
        ClearHoldForScheduler(reinterpret_cast<void*>(scheduler));
    }
    return true;
}

void BeforeDeviceReset() {
    g_holdScheduler.store(0u, std::memory_order_release);
    ResetHoldState();
}

} // namespace novafix::game::cutscene_terminal_frame::runtime

namespace novafix::game::cutscene_terminal_frame {

bool ShouldSuppressPresent() {
    return runtime::ShouldSuppressPresent();
}

void BeforeDeviceReset() {
    runtime::BeforeDeviceReset();
}

} // namespace novafix::game::cutscene_terminal_frame
