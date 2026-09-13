#pragma once

namespace novafix::input {

enum class InputSource : unsigned char {
    None,
    KeyboardMouse,
    Controller,
};

void NotifyActivity(InputSource source);
InputSource LatestActivity();
const char* InputSourceName(InputSource source);

} // namespace novafix::input
