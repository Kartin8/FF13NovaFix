#include "overlay/input/overlay_hotkey.h"

#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <limits>

namespace novafix::input {
namespace {

constexpr unsigned kNoDirectInputKey =
    (std::numeric_limits<unsigned>::max)();

} // namespace

const char* OverlayHotkeyName(settings::OverlayHotkey hotkey) {
    using Hotkey = settings::OverlayHotkey;
    switch (hotkey) {
    case Hotkey::Disabled: return "Disabled";
    case Hotkey::F8: return "F8";
    case Hotkey::F9: return "F9";
    case Hotkey::F10: return "F10";
    case Hotkey::F11: return "F11";
    case Hotkey::F12: return "F12";
    case Hotkey::Grave: return "~ (grave)";
    case Hotkey::Insert: return "Insert";
    case Hotkey::Home: return "Home";
    }
    return "Disabled";
}

int OverlayHotkeyVirtualKey(settings::OverlayHotkey hotkey) {
    using Hotkey = settings::OverlayHotkey;
    switch (hotkey) {
    case Hotkey::Disabled: return 0;
    case Hotkey::F8: return VK_F8;
    case Hotkey::F9: return VK_F9;
    case Hotkey::F10: return VK_F10;
    case Hotkey::F11: return VK_F11;
    case Hotkey::F12: return VK_F12;
    case Hotkey::Grave: return VK_OEM_3;
    case Hotkey::Insert: return VK_INSERT;
    case Hotkey::Home: return VK_HOME;
    }
    return 0;
}

unsigned OverlayHotkeyDirectInputKey(settings::OverlayHotkey hotkey) {
    using Hotkey = settings::OverlayHotkey;
    switch (hotkey) {
    case Hotkey::Disabled: return kNoDirectInputKey;
    case Hotkey::F8: return DIK_F8;
    case Hotkey::F9: return DIK_F9;
    case Hotkey::F10: return DIK_F10;
    case Hotkey::F11: return DIK_F11;
    case Hotkey::F12: return DIK_F12;
    case Hotkey::Grave: return DIK_GRAVE;
    case Hotkey::Insert: return DIK_INSERT;
    case Hotkey::Home: return DIK_HOME;
    }
    return kNoDirectInputKey;
}

bool IsOverlayHotkeyVirtualKey(int key,
                              const settings::InputSettings& settings) {
    return key != 0 &&
           (key == OverlayHotkeyVirtualKey(settings.overlayHotkey) ||
            key == OverlayHotkeyVirtualKey(settings.alternateOverlayHotkey));
}

bool IsOverlayHotkeyDirectInputKey(unsigned key,
                                  const settings::InputSettings& settings) {
    return key == OverlayHotkeyDirectInputKey(settings.overlayHotkey) ||
           key == OverlayHotkeyDirectInputKey(settings.alternateOverlayHotkey);
}

const char* OverlayGamepadHotkeyName(
    settings::OverlayGamepadHotkey hotkey) {
    using Hotkey = settings::OverlayGamepadHotkey;
    switch (hotkey) {
    case Hotkey::Disabled: return "Disabled";
    case Hotkey::StickClicks: return "L3 + R3";
    case Hotkey::ViewMenu: return "View + Menu";
    case Hotkey::Both: return "L3 + R3 or View + Menu";
    }
    return "Disabled";
}

} // namespace novafix::input
