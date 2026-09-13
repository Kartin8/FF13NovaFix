#pragma once

#include "settings/input_settings.h"

namespace novafix::input::overlay_gamepad {

bool PollToggle(settings::OverlayGamepadHotkey binding, bool menuOpen);
void FeedNavigation();
void HoldUntilReleased();
bool CapturesInput();
void ClearCapture();

} // namespace novafix::input::overlay_gamepad
