#include "game/titles/ff13_2/timing/cutscene_terminal_frame.h"

#include "game/shared/timing/cutscene_terminal_frame_runtime.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace novafix::game::xiii2_cutscene_terminal_frame {
namespace {

using cutscene_terminal_frame::runtime::Contract;
using cutscene_terminal_frame::runtime::EndpointSample;
using cutscene_terminal_frame::runtime::HookSite;

constexpr std::uint32_t kCrossFadeEnterRva = 0x008C7C10u;
constexpr std::uint32_t kClipEndRva = 0x008B02F0u;
constexpr std::uint32_t kSchedulerCleanupRva = 0x008ADFA0u;
constexpr std::uint32_t kSchedulerEnterRva = 0x008AD460u;
constexpr std::uint32_t kCommonClipEnterRva = 0x008B0220u;
constexpr std::uint32_t kCrossFadeVtableRva = 0x01F02760u;
constexpr std::uint32_t kEffectClipVtableRva = 0x01EFF3D0u;
constexpr std::uint32_t kSharedBlendEnterRva = 0x00551320u;
constexpr std::uint32_t kWhiteFadeEnterRva = 0x00551F00u;
constexpr std::uint32_t kWhiteAllClipFadeEnterRva = 0x00550180u;
constexpr std::uint32_t kFadeScreenEnterRva = 0x008C6E40u;
constexpr std::uint32_t kSecurityCookieRva = 0x01FA90DCu;
constexpr std::uint32_t kSchedulerCursorOffset = 0x98u;
constexpr float kMaximumFrameMilliseconds = 31.0f;
constexpr std::int32_t kAuthoredCutQuantum = 10000;
constexpr std::int32_t kReferenceStep = 10010;
constexpr std::int32_t kLookahead = 2 * kReferenceStep;

constexpr std::array<std::uint32_t, 1> kCameraClipVtableRvas{
    0x01E6B4D8u,
};
constexpr std::array<std::uint32_t, 1> kMotionClipVtableRvas{
    0x01E6BB40u,
};
constexpr std::array<std::uint32_t, 5> kAuthoredBlendVtableRvas{
    0x01E6F2B0u, 0x01E71230u, 0x01E6F970u, 0x01F01C70u,
    0x01E70708u,
};

constexpr std::array<std::uint8_t, 6> kCrossFadeEnterPrefix{
    0x81, 0xEC, 0x0C, 0x04, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 10> kClipEndPrefix{
    0xC7, 0x41, 0x1C, 0x03, 0x00, 0x00, 0x00, 0xC2, 0x04, 0x00,
};
constexpr std::array<std::uint8_t, 9> kSchedulerCleanupPrefix{
    0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x8B, 0xD9, 0x57,
};
constexpr std::array<std::uint8_t, 6> kSchedulerEnterPrefix{
    0x81, 0xEC, 0x38, 0x04, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 9> kCommonClipEnterPrefix{
    0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x18, 0x8B, 0x08, 0x57,
};
constexpr std::array<std::uint8_t, 5> kSharedBlendEnterPrefix{
    0xE9, 0xFB, 0xEE, 0x35, 0x00,
};
constexpr std::array<std::uint8_t, 10> kWhiteFadeEnterPrefix{
    0x83, 0xEC, 0x38, 0xA1, 0xDC, 0x90, 0x3A, 0x02, 0x33, 0xC4,
};
constexpr std::array<std::uint8_t, 9> kWhiteAllClipFadeEnterPrefix{
    0x8B, 0x44, 0x24, 0x04, 0x56, 0x50, 0x8B, 0xF1, 0xE8,
};
constexpr std::array<std::uint8_t, 9> kFadeScreenEnterPrefix{
    0x51, 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x55, 0x56, 0x57,
};

template <std::size_t Size>
std::span<const std::byte> Bytes(
    const std::array<std::uint8_t, Size>& bytes) {
    return std::as_bytes(std::span{bytes});
}

std::int32_t AdjustMotionEnd(std::int32_t start, std::int32_t end) {
    if (end - start == kAuthoredCutQuantum) {
        end += 2 * kAuthoredCutQuantum;
    }
    return end;
}

bool EndpointInLookahead(std::int64_t remaining, std::int32_t) {
    return remaining > 0 && remaining <= kLookahead &&
        remaining <= (std::numeric_limits<std::int32_t>::max)();
}

bool IsHighRefresh(float frameMilliseconds, std::int32_t) {
    return frameMilliseconds > 0.0f &&
        frameMilliseconds < kMaximumFrameMilliseconds;
}

bool CrossesTerminal(const EndpointSample& camera,
                     const EndpointSample& motion, std::int32_t) {
    const auto crossesWindow = [](const EndpointSample& sample) {
        return sample.step > 0 &&
            static_cast<std::int64_t>(sample.remaining) - sample.step <=
                kReferenceStep;
    };
    return crossesWindow(camera) && crossesWindow(motion);
}

Contract BuildContract() {
    Contract contract;
    contract.crossFadeEnter = {kCrossFadeEnterRva, Bytes(kCrossFadeEnterPrefix)};
    contract.clipEnd = {kClipEndRva, Bytes(kClipEndPrefix)};
    contract.schedulerCleanup = {
        kSchedulerCleanupRva, Bytes(kSchedulerCleanupPrefix)};
    contract.schedulerEnter = {
        kSchedulerEnterRva, Bytes(kSchedulerEnterPrefix)};
    contract.commonClipEnter = {
        kCommonClipEnterRva, Bytes(kCommonClipEnterPrefix)};
    contract.crossFadeVtableRva = kCrossFadeVtableRva;
    contract.effectClipVtableRva = kEffectClipVtableRva;
    contract.cameraClipVtableRvas = kCameraClipVtableRvas;
    contract.motionClipVtableRvas = kMotionClipVtableRvas;
    contract.schedulerCursorOffset = kSchedulerCursorOffset;
    contract.authoredBlendVtableRvas = kAuthoredBlendVtableRvas;
    contract.screenEnvBlendEnter = {
        kSharedBlendEnterRva, Bytes(kSharedBlendEnterPrefix)};
    contract.screenEnvBlendChecksVtable = true;
    contract.whiteFadeEnter = {
        kWhiteFadeEnterRva, Bytes(kWhiteFadeEnterPrefix)};
    contract.whiteAllClipFadeEnter = {
        kWhiteAllClipFadeEnterRva, Bytes(kWhiteAllClipFadeEnterPrefix)};
    contract.fadeScreenEnter = {
        kFadeScreenEnterRva, Bytes(kFadeScreenEnterPrefix)};
    contract.whiteFadeAbsoluteOperandOffset = 4u;
    contract.whiteFadeAbsoluteTargetRva = kSecurityCookieRva;
    contract.fixedReferenceStep = kReferenceStep;
    contract.endpointPolicy = {
        &AdjustMotionEnd, &EndpointInLookahead, &IsHighRefresh,
        &CrossesTerminal};
    return contract;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    cutscene_terminal_frame::runtime::Install(BuildContract());
    return TRUE;
}

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_cutscene_terminal_frame
