#pragma once

#include <windows.h>

namespace novafix::ui {

void DrawSettingsWindow(HWND gameWindow);
void FinishSettingsEdits();
void RestoreSettingsWindowState();

} // namespace novafix::ui
