#pragma once

#include "display/aspect_ratio.h"

namespace novafix::game::ff13_ui {

inline float MinimapCameraInverseAspect(
    const display::AspectRatio& aspect, float nativeInverseAspect) {
    return aspect.valid && aspect.correctionRequired
        ? 1.0f / display::kAuthoredAspectRatio
        : nativeInverseAspect;
}

} // namespace novafix::game::ff13_ui
