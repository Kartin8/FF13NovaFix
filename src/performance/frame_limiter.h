#pragma once

#include <windows.h>

namespace novafix::performance {

enum class FramePacingSite : unsigned {
    Presentation,
    Xiii2GameBoundary,
};

void PaceFrame(
    HWND gameWindow, unsigned observedRefreshRate,
    FramePacingSite site = FramePacingSite::Presentation);

} // namespace novafix::performance
