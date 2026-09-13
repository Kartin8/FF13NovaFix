#pragma once

namespace novafix::settings {

enum class PromptMode : unsigned {
    Auto,
    Controller,
    Keyboard,
};

enum class OverlayHotkey : unsigned {
    Disabled,
    F8,
    F9,
    F10,
    F11,
    F12,
    Grave,
    Insert,
    Home,
};

enum class OverlayGamepadHotkey : unsigned {
    Disabled,
    StickClicks,
    ViewMenu,
};

struct InputSettings {
    PromptMode promptMode{PromptMode::Auto};
    bool controllerVibration{true};
    unsigned vibrationStrengthPercent{100};
    OverlayHotkey overlayHotkey{OverlayHotkey::F10};
    OverlayHotkey alternateOverlayHotkey{OverlayHotkey::Grave};
    OverlayGamepadHotkey overlayGamepadHotkey{
        OverlayGamepadHotkey::StickClicks};
};

InputSettings ActiveInput();
InputSettings StoredInput();
void PreviewInput(const InputSettings& value);
bool SaveInput(const InputSettings& value);

} // namespace novafix::settings
