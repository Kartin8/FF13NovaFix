#include "overlay/input/overlay_gamepad.h"

#include "overlay/input/overlay_hotkey.h"
#include "input/xinput_backend.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "imgui.h"

namespace novafix::input::overlay_gamepad {
namespace {

struct HotkeyState {
    settings::OverlayGamepadHotkey binding{
        settings::OverlayGamepadHotkey::Disabled};
    xinput::GamepadState state{};
    ULONGLONG nextPoll{};
    bool connected{};
    bool wasDown{};
};

HotkeyState g_hotkey;
std::atomic_bool g_releasePending{false};

} // namespace

bool PollToggle(settings::OverlayGamepadHotkey binding, bool menuOpen) {
    const bool enabled = binding !=
        settings::OverlayGamepadHotkey::Disabled;
    const bool shouldPoll = enabled || menuOpen || CapturesInput();
    if (!shouldPoll) {
        g_hotkey = {};
        g_hotkey.binding = binding;
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (now < g_hotkey.nextPoll) return false;

    g_hotkey.connected = xinput::GetState(0u, g_hotkey.state);
    g_hotkey.nextPoll = now + (g_hotkey.connected ? 8u : 1000u);
    if (CapturesInput() &&
        (!g_hotkey.connected || g_hotkey.state.buttons == 0u)) {
        g_releasePending.store(false, std::memory_order_release);
    }

    constexpr std::uint16_t stickClicks =
        XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    constexpr std::uint16_t viewMenu =
        XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    const bool down = enabled &&
        (((g_hotkey.state.buttons & stickClicks) == stickClicks) ||
         ((g_hotkey.state.buttons & viewMenu) == viewMenu));
    if (g_hotkey.binding != binding) {
        g_hotkey.binding = binding;
        g_hotkey.wasDown = down;
        return false;
    }

    const bool pressed = down && !g_hotkey.wasDown;
    g_hotkey.wasDown = down;
    if (pressed) HoldUntilReleased();
    return pressed;
}

void FeedNavigation() {
    ImGuiIO& io = ImGui::GetIO();
    if (!g_hotkey.connected) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    } else {
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    }

    const xinput::GamepadState& state = g_hotkey.state;
    const auto button = [&](ImGuiKey key, std::uint16_t mask) {
        io.AddKeyEvent(key, g_hotkey.connected &&
            (state.buttons & mask) != 0u);
    };
    const auto analog = [&](ImGuiKey key, float value,
                            float minimum, float maximum) {
        const float normalized = std::clamp(
            (value - minimum) / (maximum - minimum), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(
            key, g_hotkey.connected && normalized > 0.10f,
            g_hotkey.connected ? normalized : 0.0f);
    };

    button(ImGuiKey_GamepadStart, XINPUT_GAMEPAD_START);
    button(ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK);
    button(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);
    button(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    button(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
    button(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
    button(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    button(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    button(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
    button(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    button(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
    button(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    button(ImGuiKey_GamepadL3, XINPUT_GAMEPAD_LEFT_THUMB);
    button(ImGuiKey_GamepadR3, XINPUT_GAMEPAD_RIGHT_THUMB);
    analog(ImGuiKey_GamepadL2, state.leftTrigger,
           XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255.0f);
    analog(ImGuiKey_GamepadR2, state.rightTrigger,
           XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255.0f);
    analog(ImGuiKey_GamepadLStickLeft, state.leftX,
           -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768.0f);
    analog(ImGuiKey_GamepadLStickRight, state.leftX,
           XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, 32767.0f);
    analog(ImGuiKey_GamepadLStickUp, state.leftY,
           XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, 32767.0f);
    analog(ImGuiKey_GamepadLStickDown, state.leftY,
           -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768.0f);
    analog(ImGuiKey_GamepadRStickLeft, state.rightX,
           -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, -32768.0f);
    analog(ImGuiKey_GamepadRStickRight, state.rightX,
           XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, 32767.0f);
    analog(ImGuiKey_GamepadRStickUp, state.rightY,
           XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, 32767.0f);
    analog(ImGuiKey_GamepadRStickDown, state.rightY,
           -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, -32768.0f);
}

void HoldUntilReleased() {
    g_releasePending.store(true, std::memory_order_release);
}

bool CapturesInput() {
    return g_releasePending.load(std::memory_order_acquire);
}

void ClearCapture() {
    g_releasePending.store(false, std::memory_order_release);
}

} // namespace novafix::input::overlay_gamepad
