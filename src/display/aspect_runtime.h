#pragma once

#include "display/aspect_ratio.h"

#include <cstdint>

namespace novafix::display {

void PublishBackBufferExtent(std::uint32_t width, std::uint32_t height);
AspectRatio CurrentAspectRatio();

} // namespace novafix::display
