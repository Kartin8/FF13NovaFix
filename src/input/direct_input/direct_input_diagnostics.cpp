#include "input/direct_input/direct_input_diagnostics.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

namespace novafix::input::direct_input_diagnostics {
namespace {

std::int64_t PerformanceCounterNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t PerformanceCounterFrequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

hook_profiler::Point OriginalStatePoint(DeviceClass device) {
    switch (device) {
    case DeviceClass::Controller:
        return hook_profiler::Point::DirectInputControllerGetStateOriginal;
    case DeviceClass::Keyboard:
        return hook_profiler::Point::DirectInputKeyboardGetStateOriginal;
    case DeviceClass::Mouse:
        return hook_profiler::Point::DirectInputMouseGetStateOriginal;
    case DeviceClass::Unknown:
        return hook_profiler::Point::DirectInputUnknownGetStateOriginal;
    }
    return hook_profiler::Point::DirectInputUnknownGetStateOriginal;
}

hook_profiler::Point ControllerLatencyPoint(std::uint64_t elapsedTicks) {
    const std::int64_t frequency = PerformanceCounterFrequency();
    const double milliseconds = frequency > 0
        ? static_cast<double>(elapsedTicks) * 1000.0 /
              static_cast<double>(frequency)
        : 0.0;
    if (milliseconds < 0.1) {
        return hook_profiler::Point::
            DirectInputControllerLatencyUnderPointOneMs;
    }
    if (milliseconds < 1.0) {
        return hook_profiler::Point::
            DirectInputControllerLatencyPointOneToOneMs;
    }
    if (milliseconds < 4.0) {
        return hook_profiler::Point::
            DirectInputControllerLatencyOneToFourMs;
    }
    if (milliseconds < 8.0) {
        return hook_profiler::Point::
            DirectInputControllerLatencyFourToEightMs;
    }
    if (milliseconds < 16.0) {
        return hook_profiler::Point::
            DirectInputControllerLatencyEightToSixteenMs;
    }
    return hook_profiler::Point::
        DirectInputControllerLatencySixteenMsOrMore;
}

} // namespace

StateCall BeginStateCall() {
    StateCall call{};
    call.active = IsLogEnabled(LogLevel::Debug);
    if (call.active) call.started = PerformanceCounterNow();
    return call;
}

void EndStateCall(StateCall& call, DeviceClass device,
                  bool failed, bool transientFailure) {
    if (!call.active) return;

    const std::int64_t elapsed = PerformanceCounterNow() - call.started;
    if (elapsed < 0) {
        call.active = false;
        return;
    }

    call.elapsed = static_cast<std::uint64_t>(elapsed);
    hook_profiler::RecordMeasuredTicks(
        hook_profiler::Point::DirectInputGetStateOriginal, call.elapsed);
    hook_profiler::RecordMeasuredTicks(
        OriginalStatePoint(device), call.elapsed);

    if (device != DeviceClass::Controller) return;
    hook_profiler::RecordMeasuredTicks(
        ControllerLatencyPoint(call.elapsed), call.elapsed);
    if (failed) {
        hook_profiler::RecordMeasuredTicks(
            transientFailure
                ? hook_profiler::Point::DirectInputControllerTransientFailure
                : hook_profiler::Point::DirectInputControllerOtherFailure,
            call.elapsed);
    }
}

void RecordControllerState(const StateCall& call, bool changed) {
    if (!call.active) return;
    hook_profiler::RecordMeasuredTicks(
        changed
            ? hook_profiler::Point::
                  DirectInputControllerChangedGetStateOriginal
            : hook_profiler::Point::
                  DirectInputControllerUnchangedGetStateOriginal,
        call.elapsed);
}

void LogControllerIdentity(IDirectInputDevice8A* device) {
    if (!device || !IsLogEnabled(LogLevel::Debug)) return;

    DIDEVICEINSTANCEA identity{};
    identity.dwSize = sizeof(identity);
    const HRESULT identityResult = device->GetDeviceInfo(&identity);

    DIPROPDWORD vidPid{};
    vidPid.diph.dwSize = sizeof(vidPid);
    vidPid.diph.dwHeaderSize = sizeof(vidPid.diph);
    vidPid.diph.dwHow = DIPH_DEVICE;
    const HRESULT vidPidResult = device->GetProperty(
        DIPROP_VIDPID, &vidPid.diph);
    const unsigned vendor = SUCCEEDED(vidPidResult)
        ? LOWORD(vidPid.dwData) : 0u;
    const unsigned product = SUCCEEDED(vidPidResult)
        ? HIWORD(vidPid.dwData) : 0u;

    LogDebug(
        "DirectInput controller: object=%p instance=\"%s\" product=\"%s\" "
        "vid=%04X pid=%04X instance-guid=%08lX-%04X-%04X "
        "product-guid=%08lX-%04X-%04X identity-hr=0x%08lX vidpid-hr=0x%08lX",
        device,
        SUCCEEDED(identityResult) ? identity.tszInstanceName : "",
        SUCCEEDED(identityResult) ? identity.tszProductName : "",
        vendor, product,
        static_cast<unsigned long>(identity.guidInstance.Data1),
        static_cast<unsigned>(identity.guidInstance.Data2),
        static_cast<unsigned>(identity.guidInstance.Data3),
        static_cast<unsigned long>(identity.guidProduct.Data1),
        static_cast<unsigned>(identity.guidProduct.Data2),
        static_cast<unsigned>(identity.guidProduct.Data3),
        static_cast<unsigned long>(identityResult),
        static_cast<unsigned long>(vidPidResult));
}

} // namespace novafix::input::direct_input_diagnostics
