#pragma once

#include <windows.h>

#include <vector>

namespace novafix::display {

struct MonitorDescriptor {
    HMONITOR handle{};
    RECT bounds{};
    RECT workArea{};
    bool primary{};
};

std::vector<MonitorDescriptor> EnumerateMonitors();
void InvalidateMonitorLayout();
unsigned MonitorLayoutGeneration();
MonitorDescriptor ResolveMonitor(HWND window, unsigned configuredIndex);
SIZE ConfiguredMonitorSize(unsigned configuredIndex);

} // namespace novafix::display
