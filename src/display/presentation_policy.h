#pragma once

#include "settings/performance_settings.h"

#include <d3d9.h>

namespace novafix::display {

struct PresentationPolicyContext {
    unsigned resolvedRefreshRate{};
};

void ApplyPresentationPolicy(D3DPRESENT_PARAMETERS& parameters,
                             const settings::PerformanceSettings& settings,
                             const PresentationPolicyContext& context);

bool ExternalLimiterShouldWait(const settings::PerformanceSettings& settings,
                               unsigned observedRefreshRate);

} // namespace novafix::display
