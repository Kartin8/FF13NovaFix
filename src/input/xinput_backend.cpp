#include "input/xinput_backend.h"

#include "diagnostics/log.h"

#include <windows.h>
#include <xinput.h>

#include <array>

namespace novafix::input::xinput {
namespace {

constexpr std::array<const wchar_t*, 5> kLibraries{
    L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll",
    L"xinput1_2.dll", L"xinput1_1.dll",
};

using GetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_VIBRATION*);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_module = nullptr;
GetStateFn g_getState = nullptr;
SetStateFn g_setState = nullptr;

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    for (const wchar_t* library : kLibraries) {
        HMODULE module = LoadLibraryW(library);
        if (!module) continue;
        const auto getState = reinterpret_cast<GetStateFn>(
            GetProcAddress(module, "XInputGetState"));
        const auto setState = reinterpret_cast<SetStateFn>(
            GetProcAddress(module, "XInputSetState"));
        if (getState && setState) {
            g_module = module;
            g_getState = getState;
            g_setState = setState;
            Log("XInput backend: library=%ls", library);
            return TRUE;
        }
        FreeLibrary(module);
    }
    Log("XInput backend unavailable");
    return TRUE;
}

void EnsureInitialized() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

} // namespace

bool Available() {
    EnsureInitialized();
    return g_module && g_getState && g_setState;
}

bool IsConnected(unsigned index) {
    EnsureInitialized();
    if (!g_getState || index >= XUSER_MAX_COUNT) return false;
    XINPUT_STATE state{};
    return g_getState(index, &state) == ERROR_SUCCESS;
}

unsigned ConnectedMask() {
    unsigned mask = 0;
    for (unsigned index = 0; index < XUSER_MAX_COUNT; ++index) {
        if (IsConnected(index)) mask |= 1u << index;
    }
    return mask;
}

bool GetButtons(unsigned index, std::uint16_t& buttons) {
    GamepadState state{};
    const bool connected = GetState(index, state);
    buttons = state.buttons;
    return connected;
}

bool GetState(unsigned index, GamepadState& state) {
    EnsureInitialized();
    state = {};
    if (!g_getState || index >= XUSER_MAX_COUNT) return false;
    XINPUT_STATE native{};
    if (g_getState(index, &native) != ERROR_SUCCESS) return false;
    state.buttons = native.Gamepad.wButtons;
    state.leftTrigger = native.Gamepad.bLeftTrigger;
    state.rightTrigger = native.Gamepad.bRightTrigger;
    state.leftX = native.Gamepad.sThumbLX;
    state.leftY = native.Gamepad.sThumbLY;
    state.rightX = native.Gamepad.sThumbRX;
    state.rightY = native.Gamepad.sThumbRY;
    return true;
}

bool SetVibration(unsigned index, std::uint16_t lowFrequency,
                  std::uint16_t highFrequency) {
    EnsureInitialized();
    if (!g_setState || index >= XUSER_MAX_COUNT) return false;
    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = lowFrequency;
    vibration.wRightMotorSpeed = highFrequency;
    return g_setState(index, &vibration) == ERROR_SUCCESS;
}

} // namespace novafix::input::xinput
