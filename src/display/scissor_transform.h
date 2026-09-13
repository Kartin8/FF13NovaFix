#pragma once

#include <cstdint>

namespace novafix::display {

struct ScissorRect {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};
};

ScissorRect ScaleScissorRect(ScissorRect source, std::uint32_t targetWidth,
                             std::uint32_t targetHeight);

} // namespace novafix::display
