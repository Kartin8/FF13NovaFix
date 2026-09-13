#include "display/window_geometry.h"

#include <algorithm>

namespace novafix::display {

RECT CenteredBounds(const RECT& available, unsigned width, unsigned height) {
    const LONG availableWidth = available.right - available.left;
    const LONG availableHeight = available.bottom - available.top;
    const LONG actualWidth = std::min<LONG>(static_cast<LONG>(width), availableWidth);
    const LONG actualHeight = std::min<LONG>(static_cast<LONG>(height), availableHeight);
    const LONG left = available.left + (availableWidth - actualWidth) / 2;
    const LONG top = available.top + (availableHeight - actualHeight) / 2;
    return {left, top, left + actualWidth, top + actualHeight};
}

bool SameBounds(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

LONG_PTR NormalizeTopLevelStyle(LONG_PTR current, bool decorated) {
    constexpr LONG_PTR managed = WS_POPUP | WS_CAPTION | WS_THICKFRAME |
                                 WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    const LONG_PTR required = decorated
        ? WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
        : WS_POPUP;
    return (current & ~managed) | required;
}

} // namespace novafix::display
