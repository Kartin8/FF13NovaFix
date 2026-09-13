#pragma once

#include <windows.h>

namespace novafix::display {

void InitializeDpiAwareness();
bool AdjustWindowBoundsForDpi(RECT* bounds, DWORD style, BOOL hasMenu,
                              DWORD exStyle, HWND window);

} // namespace novafix::display
