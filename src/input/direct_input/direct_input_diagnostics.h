#pragma once

#include <windows.h>

#include <cstdint>

struct IDirectInputDevice8A;

namespace novafix::input::direct_input_diagnostics {

enum class DeviceClass : std::uint8_t {
    Unknown,
    Keyboard,
    Mouse,
    Controller,
};

struct StateCall {
    std::int64_t started{};
    std::uint64_t elapsed{};
    bool active{};
};

StateCall BeginStateCall();
void EndStateCall(StateCall& call, DeviceClass device,
                  bool failed, bool transientFailure);
void RecordControllerState(const StateCall& call, bool changed);
void LogControllerIdentity(IDirectInputDevice8A* device);

} // namespace novafix::input::direct_input_diagnostics
