#pragma once

#include <cstdint>

namespace novafix::input::xinput {

struct GamepadState {
    std::uint16_t buttons{};
    std::uint8_t leftTrigger{};
    std::uint8_t rightTrigger{};
    std::int16_t leftX{};
    std::int16_t leftY{};
    std::int16_t rightX{};
    std::int16_t rightY{};
};

bool Available();
bool IsConnected(unsigned index);
unsigned ConnectedMask();
bool GetState(unsigned index, GamepadState& state);
bool GetButtons(unsigned index, std::uint16_t& buttons);
bool SetVibration(unsigned index, std::uint16_t lowFrequency,
                  std::uint16_t highFrequency);

} // namespace novafix::input::xinput
