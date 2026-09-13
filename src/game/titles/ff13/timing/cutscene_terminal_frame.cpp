#include "game/titles/ff13/timing/cutscene_terminal_frame.h"

#include "game/shared/timing/cutscene_terminal_frame_runtime.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace novafix::game::ff13_cutscene_terminal_frame {
namespace {

using cutscene_terminal_frame::runtime::Contract;
using cutscene_terminal_frame::runtime::EndpointSample;
using cutscene_terminal_frame::runtime::HookSite;

constexpr std::uint32_t kCrossFadeEnterRva = 0x00BDE310u;
constexpr std::uint32_t kClipEndRva = 0x00BAE200u;
constexpr std::uint32_t kSchedulerCleanupRva = 0x00BA4AC0u;
constexpr std::uint32_t kSchedulerEnterRva = 0x00BA4270u;
constexpr std::uint32_t kCommonClipEnterRva = 0x00BAE100u;
constexpr std::uint32_t kCrossFadeVtableRva = 0x022647A0u;
constexpr std::uint32_t kEffectClipVtableRva = 0x02260D48u;
constexpr std::uint32_t kCutReferenceStepRva = 0x0246DDBCu;
constexpr std::uint32_t kScreenEnvBlendEnterRva = 0x00724290u;
constexpr std::uint32_t kWhiteFadeEnterRva = 0x00720B30u;
constexpr std::uint32_t kWhiteAllClipFadeEnterRva = 0x0071EA40u;
constexpr std::uint32_t kFadeScreenEnterRva = 0x00BDC2A0u;
constexpr std::uint32_t kCaptureEnterRva = 0x0071F970u;
constexpr std::uint32_t kSchedulerCursorOffset = 0x90u;
constexpr float kReferenceGateMarginMilliseconds = 1.0f;

bool IsReferenceStep(std::int32_t step) {
    return step > 0 && step <= 10010;
}

bool IsEndpointInLookahead(
    std::int64_t remaining, std::int32_t referenceStep) {
    return IsReferenceStep(referenceStep) && remaining > 0 &&
        remaining <= 2LL * referenceStep &&
        remaining <= (std::numeric_limits<std::int32_t>::max)();
}

bool CrossesTerminalWindow(
    std::int32_t remaining, std::int32_t step,
    std::int32_t referenceStep) {
    return IsReferenceStep(referenceStep) &&
        static_cast<std::int64_t>(remaining) - step <= referenceStep;
}

bool EndpointsShareReferenceFrame(
    std::int32_t cameraRemaining, std::int32_t motionRemaining,
    std::int32_t referenceStep) {
    if (!IsReferenceStep(referenceStep)) return false;
    const std::int64_t difference =
        static_cast<std::int64_t>(cameraRemaining) - motionRemaining;
    const std::int64_t magnitude = difference < 0 ? -difference : difference;
    return magnitude <= referenceStep;
}

bool IsHighRefresh(float frameMilliseconds,
                   std::int32_t referenceStep) {
    const float referenceMilliseconds =
        static_cast<float>(referenceStep) / 300.0f;
    return frameMilliseconds > 0.0f &&
        frameMilliseconds + kReferenceGateMarginMilliseconds <
            referenceMilliseconds;
}

constexpr std::array<std::uint32_t, 2> kCameraClipVtableRvas{
    0x021B27C0u, 0x021B2878u,
};
constexpr std::array<std::uint32_t, 2> kMotionClipVtableRvas{
    0x021B25B0u, 0x02260FE0u,
};
constexpr std::array<std::uint32_t, 5> kAuthoredBlendVtableRvas{
    0x021B89C0u, 0x021B70F0u, 0x021B5AB0u, 0x02263DF8u,
    0x021B6548u,
};

constexpr std::array<std::uint8_t, 9> kCrossFadeEnterPrefix{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xB4, 0x08, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 10> kClipEndPrefix{
    0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC, 0x8B, 0x45, 0xFC,
};
constexpr std::array<std::uint8_t, 8> kSchedulerCleanupPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x50, 0x56, 0x57,
};
constexpr std::array<std::uint8_t, 9> kSchedulerEnterPrefix{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xC4, 0x04, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 10> kCommonClipEnterPrefix{
    0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC, 0x8B, 0x45, 0xFC,
};
constexpr std::array<std::uint8_t, 7> kScreenEnvBlendEnterPrefix{
    0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC,
};
constexpr std::array<std::uint8_t, 9> kWhiteFadeEnterPrefix{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x70, 0x01, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 7> kWhiteAllClipFadeEnterPrefix{
    0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC,
};
constexpr std::array<std::uint8_t, 9> kFadeScreenEnterPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x54, 0x89, 0x4D, 0xB4,
};
constexpr std::array<std::uint8_t, 6> kCaptureEnterPrefix{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x60,
};

template <std::size_t Size>
std::span<const std::byte> Bytes(
    const std::array<std::uint8_t, Size>& bytes) {
    return std::as_bytes(std::span{bytes});
}

std::int32_t KeepMotionEnd(std::int32_t, std::int32_t end) {
    return end;
}

bool EndpointInLookahead(
    std::int64_t remaining, std::int32_t referenceStep) {
    return IsEndpointInLookahead(remaining, referenceStep);
}

bool CrossesTerminal(const EndpointSample& camera,
                     const EndpointSample& motion,
                     std::int32_t referenceStep) {
    return CrossesTerminalWindow(
               camera.remaining, camera.step, referenceStep) &&
        CrossesTerminalWindow(
               motion.remaining, motion.step, referenceStep) &&
        EndpointsShareReferenceFrame(
               camera.remaining, motion.remaining, referenceStep);
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
        kScreenEnvBlendEnterRva, Bytes(kScreenEnvBlendEnterPrefix)};
    contract.whiteFadeEnter = {
        kWhiteFadeEnterRva, Bytes(kWhiteFadeEnterPrefix)};
    contract.whiteAllClipFadeEnter = {
        kWhiteAllClipFadeEnterRva, Bytes(kWhiteAllClipFadeEnterPrefix)};
    contract.fadeScreenEnter = {
        kFadeScreenEnterRva, Bytes(kFadeScreenEnterPrefix)};
    contract.captureEnter = {kCaptureEnterRva, Bytes(kCaptureEnterPrefix)};
    contract.liveReferenceStepRva = kCutReferenceStepRva;
    contract.effectClipFallback = true;
    contract.endpointPolicy = {
        &KeepMotionEnd, &EndpointInLookahead, &IsHighRefresh,
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

} // namespace novafix::game::ff13_cutscene_terminal_frame
