#pragma once

namespace novafix::input {

void InstallWin32Hooks();
void FeedOverlayMouse();
// Use the native Win32 cursor so overlay movement is not tied to Present
void SetOverlayNativeCursorVisible(bool visible);
bool GameInputHasFocus();
bool GameInputSuppressed();

} // namespace novafix::input
