#include "display/monitor_layout.h"

#include <algorithm>
#include <atomic>

namespace novafix::display {
namespace {

std::atomic_uint g_generation{1};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* monitors = reinterpret_cast<std::vector<MonitorDescriptor>*>(context);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    monitors->push_back({monitor, info.rcMonitor, info.rcWork,
                         (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

bool MonitorOrder(const MonitorDescriptor& left, const MonitorDescriptor& right) {
    if (left.primary != right.primary) return left.primary;
    if (left.bounds.top != right.bounds.top) return left.bounds.top < right.bounds.top;
    return left.bounds.left < right.bounds.left;
}

MonitorDescriptor FromHandle(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return {};
    return {monitor, info.rcMonitor, info.rcWork,
            (info.dwFlags & MONITORINFOF_PRIMARY) != 0};
}

} // namespace

std::vector<MonitorDescriptor> EnumerateMonitors() {
    std::vector<MonitorDescriptor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor,
                        reinterpret_cast<LPARAM>(&monitors));
    std::sort(monitors.begin(), monitors.end(), &MonitorOrder);
    return monitors;
}

void InvalidateMonitorLayout() {
    g_generation.fetch_add(1u, std::memory_order_release);
}

unsigned MonitorLayoutGeneration() {
    return g_generation.load(std::memory_order_acquire);
}

MonitorDescriptor ResolveMonitor(HWND window, unsigned configuredIndex) {
    const std::vector<MonitorDescriptor> monitors = EnumerateMonitors();
    if (configuredIndex > 0 && configuredIndex <= monitors.size()) {
        return monitors[configuredIndex - 1];
    }
    const HMONITOR nearest = window
        ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MonitorDescriptor result = FromHandle(nearest);
    if (!result.handle && !monitors.empty()) result = monitors.front();
    return result;
}

SIZE ConfiguredMonitorSize(unsigned configuredIndex) {
    const MonitorDescriptor monitor = ResolveMonitor(nullptr, configuredIndex);
    return {monitor.bounds.right - monitor.bounds.left,
            monitor.bounds.bottom - monitor.bounds.top};
}

} // namespace novafix::display
