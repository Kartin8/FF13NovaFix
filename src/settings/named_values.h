#pragma once

namespace novafix::settings::names {

inline constexpr const wchar_t* kDisplayMode[]{
    L"Windowed", L"Borderless", L"BorderlessFullscreen"};
inline constexpr const wchar_t* kMonitor[]{L"Automatic"};
inline constexpr const wchar_t* kFrameRateLimit[]{L"Unlimited"};
inline constexpr const wchar_t* kVSyncMode[]{L"GameDefault", L"Off", L"On"};
inline constexpr const wchar_t* kRefreshRate[]{L"Automatic"};
inline constexpr const wchar_t* kTripleBuffering[]{L"Automatic", L"Off", L"On"};
inline constexpr const wchar_t* kD3D9Backend[]{
    L"Automatic", L"System", L"LocalDXVK"};
inline constexpr const wchar_t* kLoadedModulesNotificationMode[]{
    L"Never", L"WhenChanged", L"EveryLaunch"};
inline constexpr const wchar_t* kPromptMode[]{
    L"Automatic", L"Controller", L"Keyboard"};
inline constexpr const wchar_t* kOverlayHotkey[]{
    L"Disabled", L"F8", L"F9", L"F10", L"F11", L"F12", L"Grave",
    L"Insert", L"Home"};
inline constexpr const wchar_t* kOverlayGamepadHotkey[]{
    L"Disabled", L"StickClicks", L"ViewMenu"};

} // namespace novafix::settings::names
