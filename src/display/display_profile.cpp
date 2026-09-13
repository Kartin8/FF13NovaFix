#include "display/display_profile.h"

#include "diagnostics/log.h"
#include "display/dpi_awareness.h"
#include "display/monitor_layout.h"
#include "display/window_geometry.h"
#include "settings/graphics_settings.h"

#include <d3d9.h>

#include <atomic>

namespace novafix::display {
namespace {

std::atomic_bool g_environmentDirty{true};
std::atomic<ULONGLONG> g_lastMaintenance{};
std::atomic<settings::DisplayMode> g_effectiveDisplayMode{
    settings::DisplayMode::Windowed};
std::atomic_bool g_initialWindowActivated{};
std::atomic_bool g_bootstrapActivationDeferredLogged{};

bool ApplyStyle(HWND window, bool decorated) {
    const LONG_PTR current = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR desired = NormalizeTopLevelStyle(current, decorated);
    if (current == desired) return false;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(window, GWL_STYLE, desired);
    return previous != 0 || GetLastError() == ERROR_SUCCESS;
}

bool ApplyBounds(HWND window, const RECT& desired, bool frameChanged) {
    RECT current{};
    if (GetWindowRect(window, &current) && SameBounds(current, desired) && !frameChanged) {
        return false;
    }
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING;
    if (frameChanged) flags |= SWP_FRAMECHANGED;
    return SetWindowPos(window, nullptr, desired.left, desired.top,
                        desired.right - desired.left, desired.bottom - desired.top,
                        flags) != FALSE;
}

bool SetDecoratedWindow(HWND window, UINT width, UINT height, bool preservePosition) {
    if (!window || !IsWindow(window)) return false;
    const bool styleChanged = ApplyStyle(window, true);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT frame{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (!AdjustWindowBoundsForDpi(&frame, style, GetMenu(window) != nullptr,
                                  exStyle, window)) {
        return styleChanged;
    }
    RECT current{};
    GetWindowRect(window, &current);
    const LONG left = preservePosition ? current.left : frame.left;
    const LONG top = preservePosition ? current.top : frame.top;
    const RECT desired{left, top, left + frame.right - frame.left,
                       top + frame.bottom - frame.top};
    return ApplyBounds(window, desired, styleChanged) || styleChanged;
}

bool SetBorderlessWindow(HWND window, const RECT& bounds) {
    if (!window || !IsWindow(window)) return false;
    const bool styleChanged = ApplyStyle(window, false);
    return ApplyBounds(window, bounds, styleChanged) || styleChanged;
}

bool RepairWindow(HWND window, const settings::GraphicsSettings& profile) {
    const settings::DisplayMode displayMode =
        g_effectiveDisplayMode.load(std::memory_order_acquire);
    switch (displayMode) {
    case settings::DisplayMode::Windowed:
        // The game owns windowed placement after CreateDevice; reapplying bounds fights it
        return false;
    case settings::DisplayMode::Borderless:
    case settings::DisplayMode::BorderlessFullscreen:
        break;
    }

    const MonitorDescriptor monitor = ResolveMonitor(window, profile.monitorIndex);
    if (!monitor.handle) return false;
    switch (displayMode) {
    case settings::DisplayMode::Borderless:
        return SetBorderlessWindow(
            window, CenteredBounds(monitor.workArea, profile.width, profile.height));
    case settings::DisplayMode::BorderlessFullscreen:
        return SetBorderlessWindow(window, monitor.bounds);
    case settings::DisplayMode::Windowed:
        return false;
    }
    return false;
}

} // namespace

HWND PresentationWindow(HWND focusWindow, const D3DPRESENT_PARAMETERS* parameters) {
    if (parameters && parameters->hDeviceWindow) return parameters->hDeviceWindow;
    return focusWindow;
}

bool ShouldApplyProfile(const D3DPRESENT_PARAMETERS* parameters) {
    return settings::OverridesLauncherGraphics() && parameters &&
           parameters->BackBufferWidth >= 640 && parameters->BackBufferHeight >= 480;
}

void ApplyProfile(HWND focusWindow, D3DPRESENT_PARAMETERS* parameters) {
    if (!parameters) return;
    const settings::GraphicsSettings profile = settings::Active();
    const settings::DisplayMode displayMode = profile.displayMode;
    g_effectiveDisplayMode.store(displayMode, std::memory_order_release);
    const HWND window = PresentationWindow(focusWindow, parameters);
    const MonitorDescriptor monitor = ResolveMonitor(window, profile.monitorIndex);

    parameters->BackBufferWidth = profile.width;
    parameters->BackBufferHeight = profile.height;
    parameters->FullScreen_RefreshRateInHz = 0;

    switch (displayMode) {
    case settings::DisplayMode::Windowed:
        parameters->Windowed = TRUE;
        SetDecoratedWindow(window, profile.width, profile.height, true);
        break;
    case settings::DisplayMode::Borderless:
        parameters->Windowed = TRUE;
        if (monitor.handle) {
            SetBorderlessWindow(window,
                CenteredBounds(monitor.workArea, profile.width, profile.height));
        }
        break;
    case settings::DisplayMode::BorderlessFullscreen:
        parameters->Windowed = TRUE;
        if (monitor.handle) {
            parameters->BackBufferWidth = static_cast<UINT>(monitor.bounds.right - monitor.bounds.left);
            parameters->BackBufferHeight = static_cast<UINT>(monitor.bounds.bottom - monitor.bounds.top);
            SetBorderlessWindow(window, monitor.bounds);
        }
        break;
    }
    g_environmentDirty.store(false, std::memory_order_release);
}

void ActivateInitialWindow(HWND window) {
    if (g_initialWindowActivated.load(std::memory_order_acquire)) return;
    if (!window || !IsWindow(window)) return;

    RECT client{};
    if (!GetClientRect(window, &client)) return;
    const LONG clientWidth = client.right - client.left;
    const LONG clientHeight = client.bottom - client.top;
    // LR starts with a 1x1 client area; defer activation until Reset gives it a usable surface
    if (clientWidth < 640 || clientHeight < 480) {
        if (!g_bootstrapActivationDeferredLogged.exchange(
                true, std::memory_order_acq_rel)) {
            Log("Initial window activation deferred: hwnd=%p client=%ldx%ld visible=%d",
                window, clientWidth, clientHeight,
                IsWindowVisible(window) ? 1 : 0);
        }
        return;
    }
    if (g_initialWindowActivated.exchange(true, std::memory_order_acq_rel)) return;

    RECT current{};
    const bool haveBounds = GetWindowRect(window, &current) != FALSE;
    const settings::GraphicsSettings profile = settings::Active();
    const MonitorDescriptor monitor = ResolveMonitor(window, profile.monitorIndex);

    LONG left = 0;
    LONG top = 0;
    bool center = haveBounds && monitor.handle;
    if (center) {
        const LONG width = current.right - current.left;
        const LONG height = current.bottom - current.top;
        const LONG monitorWidth = monitor.bounds.right - monitor.bounds.left;
        const LONG monitorHeight = monitor.bounds.bottom - monitor.bounds.top;
        const RECT& area = width >= monitorWidth && height >= monitorHeight
            ? monitor.bounds : monitor.workArea;
        left = area.left + ((area.right - area.left) - width) / 2;
        top = area.top + ((area.bottom - area.top) - height) / 2;
    }

    UINT flags = SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING |
                 SWP_SHOWWINDOW;
    if (!center) flags |= SWP_NOMOVE;
    SetWindowPos(window, HWND_TOP, left, top, 0, 0, flags);
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);
}

void NotifyWindowEnvironmentChanged() {
    InvalidateMonitorLayout();
    g_environmentDirty.store(true, std::memory_order_release);
}

void MaintainProfile(HWND window) {
    if (!settings::OverridesLauncherGraphics()) return;
    const ULONGLONG now = GetTickCount64();
    const bool dirty = g_environmentDirty.load(std::memory_order_acquire);
    const ULONGLONG previous = g_lastMaintenance.load(std::memory_order_relaxed);
    if (!dirty && now - previous < 500) return;
    // Window validation crosses into USER32. Keep it off the ordinary Present
    // path and pay for it only on the periodic/event-driven maintenance pass
    if (!window || !IsWindow(window) || IsIconic(window)) return;
    if (dirty) {
        bool expected = true;
        g_environmentDirty.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel);
    }
    g_lastMaintenance.store(now, std::memory_order_relaxed);
    if (RepairWindow(window, settings::Active())) {
        Log("Window profile repaired after focus, DPI, or monitor topology change");
    }
}

} // namespace novafix::display
