#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace novafix::game::cutscene_terminal_frame::runtime {

struct HookSite {
    std::uint32_t rva{};
    std::span<const std::byte> expected;

    explicit operator bool() const {
        return rva != 0u && !expected.empty();
    }
};

struct EndpointSample {
    std::int32_t remaining{};
    std::int32_t step{};
    bool approaching{};
};

using AdjustMotionEndFn = std::int32_t(*)(std::int32_t start,
                                          std::int32_t end);
using EndpointInLookaheadFn = bool(*)(std::int64_t remaining,
                                      std::int32_t referenceStep);
using HighRefreshFn = bool(*)(float frameMilliseconds,
                              std::int32_t referenceStep);
using TerminalCrossingFn = bool(*)(const EndpointSample& camera,
                                   const EndpointSample& motion,
                                   std::int32_t referenceStep);

struct EndpointPolicy {
    AdjustMotionEndFn adjustMotionEnd{};
    EndpointInLookaheadFn endpointInLookahead{};
    HighRefreshFn highRefresh{};
    TerminalCrossingFn crossesTerminal{};
};

struct Contract {
    HookSite crossFadeEnter;
    HookSite clipEnd;
    HookSite schedulerCleanup;
    HookSite schedulerEnter;
    HookSite commonClipEnter;

    std::uint32_t crossFadeVtableRva{};
    std::uint32_t effectClipVtableRva{};
    std::span<const std::uint32_t> cameraClipVtableRvas;
    std::span<const std::uint32_t> motionClipVtableRvas;
    std::uint32_t schedulerCursorOffset{};

    std::span<const std::uint32_t> authoredBlendVtableRvas;
    HookSite screenEnvBlendEnter;
    bool screenEnvBlendChecksVtable{};
    HookSite whiteFadeEnter;
    HookSite whiteAllClipFadeEnter;
    HookSite fadeScreenEnter;
    HookSite captureEnter;

    std::uint32_t whiteFadeAbsoluteOperandOffset{};
    std::uint32_t whiteFadeAbsoluteTargetRva{};

    std::int32_t fixedReferenceStep{};
    std::uint32_t liveReferenceStepRva{};
    bool effectClipFallback{};
    EndpointPolicy endpointPolicy;
};

bool Install(const Contract& contract);
bool ShouldSuppressPresent();
void BeforeDeviceReset();

} // namespace novafix::game::cutscene_terminal_frame::runtime
