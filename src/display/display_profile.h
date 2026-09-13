#pragma once

#include <windows.h>
#include <d3d9.h>

namespace novafix::display {

HWND PresentationWindow(HWND focusWindow, const D3DPRESENT_PARAMETERS* parameters);
bool ShouldApplyProfile(const D3DPRESENT_PARAMETERS* parameters);
void ApplyProfile(HWND focusWindow, D3DPRESENT_PARAMETERS* parameters);
void ActivateInitialWindow(HWND window);
void NotifyWindowEnvironmentChanged();
void MaintainProfile(HWND window);

} // namespace novafix::display
