#pragma once

#include "display/aspect_ratio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace novafix::game::ff13_ui {

enum class BackdropMapping { CoverArt, CapturedViewport };

struct BackdropVertex {
    float x;
    float y;
    std::array<std::byte, 24> remaining;
};
static_assert(sizeof(BackdropVertex) == 32u);

inline bool FitBackdropQuad(std::span<BackdropVertex, 4> vertices,
                            const display::AspectRatio& aspect,
                            BackdropMapping mapping) {
    if (!aspect.valid || !aspect.correctionRequired ||
        !std::isfinite(aspect.safeScaleX) ||
        !std::isfinite(aspect.safeScaleY) ||
        aspect.safeScaleX <= 0.0f || aspect.safeScaleY <= 0.0f) return false;

    float scaleX = 1.0f / aspect.safeScaleX;
    float scaleY = 1.0f / aspect.safeScaleY;
    if (mapping == BackdropMapping::CoverArt) {
        scaleX = scaleY = std::max(scaleX, scaleY);
    }
    std::array<std::array<float, 2>, 4> positions{};
    for (std::size_t index = 0; index != vertices.size(); ++index) {
        positions[index] = {
            640.0f + (vertices[index].x - 640.0f) * scaleX,
            360.0f + (vertices[index].y - 360.0f) * scaleY,
        };
        if (!std::isfinite(positions[index][0]) ||
            !std::isfinite(positions[index][1])) return false;
    }
    for (std::size_t index = 0; index != vertices.size(); ++index) {
        vertices[index].x = positions[index][0];
        vertices[index].y = positions[index][1];
    }
    return true;
}

} // namespace novafix::game::ff13_ui
