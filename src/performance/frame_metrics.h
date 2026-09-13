#pragma once

#include "performance/frame_statistics.h"

#include <windows.h>

struct IDirect3DDevice9;

namespace novafix::performance {

void BeginFrame(IDirect3DDevice9* device, HWND window);
void EndFrame(HRESULT result);
FrameStatisticsSnapshot CurrentFrameStatistics();
unsigned ObservedRefreshRate();
float CurrentFrameMilliseconds();
unsigned CurrentTargetRate();

} // namespace novafix::performance
