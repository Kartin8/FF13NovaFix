#pragma once

#include <windows.h>

#include <string_view>

struct IDirect3DDevice9;

namespace novafix::overlay {

void SelectPresentationWindow(HWND window);
HWND PresentationWindow();

bool IsOpen();
bool WantsInput();
bool CapturesInput();
bool CapturesControllerInput();
void CloseForFocusLoss();
// Stop submitting ImGui frames while the game processes a window-close
// request.  This is deliberately temporary: some of the games may cancel a
// native close confirmation and continue running
void PrepareForApplicationClose();
bool ForwardWin32InputMessage(HWND window, UINT message, WPARAM wParam,
                              LPARAM lParam);

void OnPresent(IDirect3DDevice9* device);
void ShowNotification(std::string_view text, ULONGLONG durationMs = 5000u);
bool OwnsDevice(IDirect3DDevice9* device);
void DetachDevice(IDirect3DDevice9* device);
void BeforeDeviceReset(IDirect3DDevice9* device);
void AfterDeviceReset(IDirect3DDevice9* device, HRESULT result);

} // namespace novafix::overlay
