#pragma once

#include "display/aspect_ratio.h"

#include <array>
#include <cstddef>

namespace novafix::game::map_alert_transform {

// Keep the VFX's own size adjustment, then apply the same safe-area transform as the minimap.
inline void Apply(std::array<float, 16>& matrix,
                  const display::AspectRatio& aspect,
                  float scaleX, float scaleY) {
    for (std::size_t row = 0; row < 4; ++row) {
        matrix[row] *= scaleX;
        matrix[4u + row] *= scaleY;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        matrix[4u * column] *= aspect.safeScaleX;
        matrix[4u * column + 1u] *= aspect.safeScaleY;
    }
}

} // namespace novafix::game::map_alert_transform
