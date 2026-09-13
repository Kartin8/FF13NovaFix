#pragma once

#include <windows.h>

namespace novafix::display {

RECT CenteredBounds(const RECT& available, unsigned width, unsigned height);
bool SameBounds(const RECT& left, const RECT& right);
LONG_PTR NormalizeTopLevelStyle(LONG_PTR current, bool decorated);

} // namespace novafix::display
