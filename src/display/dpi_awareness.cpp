#include "display/dpi_awareness.h"

#include "compat/platform.h"
#include "diagnostics/log.h"

#include <windows.h>

namespace novafix::display {
namespace {

using SetProcessDpiAwarenessContextFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
using AdjustWindowRectExForDpiFn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

template <typename Function>
Function User32Function(const char* name) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<Function>(GetProcAddress(user32, name)) : nullptr;
}

} // namespace

void InitializeDpiAwareness() {
    if (compat::IsWine()) {
        return;
    }

    const auto setContext =
        User32Function<SetProcessDpiAwarenessContextFn>("SetProcessDpiAwarenessContext");
    if (setContext) {
        SetLastError(ERROR_SUCCESS);
        if (setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            return;
        }
    }
    if (!SetProcessDPIAware()) {
        Log("DPI awareness request was not accepted: error=%lu preserving=host-policy",
            GetLastError());
    }
}

bool AdjustWindowBoundsForDpi(RECT* bounds, DWORD style, BOOL hasMenu,
                              DWORD exStyle, HWND window) {
    if (!bounds) return false;
    const auto adjust =
        User32Function<AdjustWindowRectExForDpiFn>("AdjustWindowRectExForDpi");
    const auto getDpi = User32Function<GetDpiForWindowFn>("GetDpiForWindow");
    if (adjust) {
        const UINT dpi = window && getDpi ? getDpi(window) : USER_DEFAULT_SCREEN_DPI;
        if (adjust(bounds, style, hasMenu, exStyle,
                   dpi ? dpi : USER_DEFAULT_SCREEN_DPI)) {
            return true;
        }
    }
    return AdjustWindowRectEx(bounds, style, hasMenu, exStyle) != FALSE;
}

} // namespace novafix::display
