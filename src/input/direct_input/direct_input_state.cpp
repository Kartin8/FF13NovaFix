#include "input/direct_input/direct_input_state.h"

#include "diagnostics/log.h"
#include "overlay/input/overlay_hotkey.h"
#include "settings/input_settings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace novafix::input {

DirectInputStateTracker::Sample DirectInputStateTracker::BeginSample(
    IDirectInputDevice8A* device, DWORD size, const void* data,
    unsigned deviceKind) {
    Sample sample{};
    if (!data) return sample;

    sample.current = static_cast<const unsigned char*>(data);
    sample.size = (std::min)(size, static_cast<DWORD>(previous_.size()));
    sample.comparable = hasPrevious_ && previousSize_ == sample.size;
    if (!loggedSize_) {
        Log("DirectInput state size: object=%p kind=%u size=%lu",
            device, deviceKind, size);
        loggedSize_ = true;
    }
    return sample;
}

void DirectInputStateTracker::CommitSample(const Sample& sample) {
    if (!sample.current) return;
    std::memcpy(previous_.data(), sample.current, sample.size);
    previousSize_ = sample.size;
    hasPrevious_ = true;
}

DirectInputStateObservation DirectInputStateTracker::ObserveKeyboard(
    IDirectInputDevice8A* device, DWORD size, const void* data,
    unsigned deviceKind) {
    const Sample sample = BeginSample(device, size, data, deviceKind);
    DirectInputStateObservation observation{};
    observation.comparable = sample.comparable;

    if (sample.current && sample.comparable) {
        const settings::InputSettings inputSettings = settings::ActiveInput();
        const unsigned primary =
            OverlayHotkeyDirectInputKey(inputSettings.overlayHotkey);
        const unsigned alternate =
            OverlayHotkeyDirectInputKey(inputSettings.alternateOverlayHotkey);
        for (DWORD index = 0; index < sample.size; ++index) {
            if (index == primary || index == alternate) continue;
            if ((sample.current[index] & 0x80u) != 0 &&
                (previous_[index] & 0x80u) == 0) {
                observation.active = true;
                break;
            }
        }
    }

    CommitSample(sample);
    return observation;
}

DirectInputStateObservation DirectInputStateTracker::ObserveMouse(
    IDirectInputDevice8A* device, DWORD size, const void* data,
    unsigned deviceKind) {
    const Sample sample = BeginSample(device, size, data, deviceKind);
    DirectInputStateObservation observation{};
    observation.comparable = sample.comparable;

    if (sample.current && sample.size >= 3u * sizeof(LONG)) {
        const auto* axes = reinterpret_cast<const LONG*>(sample.current);
        observation.active = axes[0] != 0 || axes[1] != 0 || axes[2] != 0;
    }

    const DWORD buttonOffset =
        static_cast<DWORD>(offsetof(DIMOUSESTATE, rgbButtons));
    if (!observation.active && sample.current && sample.comparable &&
        sample.size > buttonOffset) {
        for (DWORD index = buttonOffset; index < sample.size; ++index) {
            if ((sample.current[index] & 0x80u) != 0 &&
                (previous_[index] & 0x80u) == 0) {
                observation.active = true;
                break;
            }
        }
    }

    CommitSample(sample);
    return observation;
}

DirectInputStateObservation DirectInputStateTracker::ObserveController(
    IDirectInputDevice8A* device, DWORD size, const void* data,
    unsigned deviceKind, bool classifyChange) {
    const Sample sample = BeginSample(device, size, data, deviceKind);
    DirectInputStateObservation observation{};
    observation.comparable = sample.comparable;
    if (sample.current && classifyChange && sample.comparable) {
        observation.changed =
            std::memcmp(sample.current, previous_.data(), sample.size) != 0;
    }
    if (!sample.current || sample.size < sizeof(DIJOYSTATE)) {
        CommitSample(sample);
        return observation;
    }

    const auto* current = reinterpret_cast<const DIJOYSTATE*>(sample.current);
    const auto* previous = reinterpret_cast<const DIJOYSTATE*>(previous_.data());
    if (sample.comparable) {
        for (std::size_t index = 0;
             index < std::size(current->rgbButtons); ++index) {
            if ((current->rgbButtons[index] & 0x80u) != 0 &&
                (previous->rgbButtons[index] & 0x80u) == 0) {
                observation.active = true;
            }
        }
        for (std::size_t index = 0;
             index < std::size(current->rgdwPOV); ++index) {
            if (current->rgdwPOV[index] != 0xFFFFFFFFu &&
                current->rgdwPOV[index] != previous->rgdwPOV[index]) {
                observation.active = true;
            }
        }
    }

    constexpr std::size_t axisCount =
        offsetof(DIJOYSTATE, rgdwPOV) / sizeof(LONG);
    static_assert(sizeof(LONG) == sizeof(std::int32_t));
    const auto* currentAxes =
        reinterpret_cast<const std::int32_t*>(current);
    observation.active = axes_.Observe({currentAxes, axisCount}) ||
        observation.active;

    if (!hasNeutralAxes_) {
        std::copy_n(currentAxes, neutralAxes_.size(), neutralAxes_.begin());
        hasNeutralAxes_ = true;
    }

    CommitSample(sample);
    return observation;
}

void DirectInputStateTracker::NeutralizeController(
    DWORD size, void* data) const {
    DirectInputNeutralAxes neutral = neutralAxes_;
    bool calibrated = hasNeutralAxes_;
    if (!calibrated) {
        // Crystal Tools uses 0..255 axes; unfocused devices may have no sample to calibrate from
        neutral.fill(128);
        calibrated = true;
    }
    NeutralizeControllerState(data, size, neutral, calibrated);
}

void DirectInputStateTracker::Reset() {
    previousSize_ = 0u;
    hasPrevious_ = false;
    loggedSize_ = false;
    axes_.Reset();
    neutralAxes_.fill(0);
    hasNeutralAxes_ = false;
}

} // namespace novafix::input
