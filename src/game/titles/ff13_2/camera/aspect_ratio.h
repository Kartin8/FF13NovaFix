#pragma once

#include <array>

namespace novafix::game::xiii2_aspect_ratio {

struct MinimapClipCorrection {
    // c254: map-fill geometry and mask Y; c255: inverse camera correction.
    std::array<float, 8> constants{
        1.0f, 0.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 0.0f, 0.0f};
    bool active{};
};

void Initialize();
MinimapClipCorrection CurrentMinimapClipCorrection();

} // namespace novafix::game::xiii2_aspect_ratio
