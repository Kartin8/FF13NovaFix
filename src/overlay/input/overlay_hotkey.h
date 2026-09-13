#pragma once

#include "settings/input_settings.h"

#include <cstdint>

namespace novafix::input {

const char* OverlayHotkeyName(settings::OverlayHotkey hotkey);
int OverlayHotkeyVirtualKey(settings::OverlayHotkey hotkey);
unsigned OverlayHotkeyDirectInputKey(settings::OverlayHotkey hotkey);
bool IsOverlayHotkeyVirtualKey(int key,
                              const settings::InputSettings& settings);
bool IsOverlayHotkeyDirectInputKey(unsigned key,
                                  const settings::InputSettings& settings);
const char* OverlayGamepadHotkeyName(
    settings::OverlayGamepadHotkey hotkey);
std::uint16_t OverlayGamepadHotkeyMask(
    settings::OverlayGamepadHotkey hotkey);

} // namespace novafix::input
