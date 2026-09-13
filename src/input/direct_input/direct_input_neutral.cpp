#include "input/direct_input/direct_input_neutral.h"

#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace novafix::input {

bool NeutralizeControllerState(void* data, std::size_t size,
                               const DirectInputNeutralAxes& calibratedAxes,
                               bool calibrated) {
    if (!data || size < sizeof(DIJOYSTATE)) return false;

    auto* state = static_cast<DIJOYSTATE*>(data);
    DirectInputNeutralAxes neutral = calibratedAxes;
    if (!calibrated) {
        const auto* current = reinterpret_cast<const std::int32_t*>(state);
        const bool compact = std::all_of(current, current + kDirectInputAxisCount,
                                         [](std::int32_t value) {
                                             return value >= 0 && value <= 255;
                                         });
        neutral.fill(compact ? 128 : 32767);
    }

    ZeroMemory(data, size);
    std::memcpy(state, neutral.data(), sizeof(neutral));
    std::fill(std::begin(state->rgdwPOV), std::end(state->rgdwPOV), 0xFFFFFFFFu);
    return true;
}

} // namespace novafix::input
