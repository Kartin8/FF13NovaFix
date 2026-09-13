#pragma once

#include "input/axis_activity.h"
#include "input/direct_input/direct_input_neutral.h"

#include <windows.h>

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>

#include <array>

namespace novafix::input {

struct DirectInputStateObservation {
    bool active{};
    bool comparable{};
    bool changed{};
};

class DirectInputStateTracker {
public:
    DirectInputStateObservation ObserveKeyboard(
        IDirectInputDevice8A* device, DWORD size, const void* data,
        unsigned deviceKind);
    DirectInputStateObservation ObserveMouse(
        IDirectInputDevice8A* device, DWORD size, const void* data,
        unsigned deviceKind);
    DirectInputStateObservation ObserveController(
        IDirectInputDevice8A* device, DWORD size, const void* data,
        unsigned deviceKind, bool classifyChange);

    void NeutralizeController(DWORD size, void* data) const;
    void Reset();

private:
    struct Sample {
        const unsigned char* current{};
        DWORD size{};
        bool comparable{};
    };

    Sample BeginSample(IDirectInputDevice8A* device, DWORD size,
                       const void* data, unsigned deviceKind);
    void CommitSample(const Sample& sample);

    std::array<unsigned char, sizeof(DIJOYSTATE2)> previous_{};
    DWORD previousSize_{};
    bool hasPrevious_{};
    bool loggedSize_{};
    AxisActivityTracker axes_;
    DirectInputNeutralAxes neutralAxes_{};
    bool hasNeutralAxes_{};
};

} // namespace novafix::input
