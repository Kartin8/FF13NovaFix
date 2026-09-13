#include "display/presentation_policy.h"

#include <algorithm>

namespace novafix::display {

void ApplyPresentationPolicy(D3DPRESENT_PARAMETERS& parameters,
                             const settings::PerformanceSettings& settings,
                             const PresentationPolicyContext& context) {
    switch (settings.vsyncMode) {
    case settings::VSyncMode::GameDefault:
        break;
    case settings::VSyncMode::Off:
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        break;
    case settings::VSyncMode::On:
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        break;
    }

    parameters.FullScreen_RefreshRateInHz =
        parameters.Windowed ? 0u : context.resolvedRefreshRate;

    if (settings.tripleBuffering == settings::TripleBufferingMode::On) {
        parameters.BackBufferCount = 2;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    } else if (settings.tripleBuffering == settings::TripleBufferingMode::Off) {
        parameters.BackBufferCount = 1;
    }
}

bool ExternalLimiterShouldWait(const settings::PerformanceSettings& settings,
                               unsigned observedRefreshRate) {
    if (settings.frameRateLimit == 0) return false;
    if (settings.vsyncMode != settings::VSyncMode::On || observedRefreshRate == 0) return true;
    return settings.frameRateLimit < observedRefreshRate;
}

} // namespace novafix::display
