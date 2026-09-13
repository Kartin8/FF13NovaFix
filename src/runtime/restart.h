#pragma once

#include <windows.h>

namespace novafix::restart {

constexpr DWORD kForcedRestartExitCode = 0x4E465852u;

void SetModuleHandle(HMODULE module);
bool IsHelperProcess();
bool ScheduleCurrentProcessRestart(HWND gameWindow);
void RunHelper();

} // namespace novafix::restart
