#pragma once

#include <cstdint>

namespace novafix::display::texture_policy {

// Preserves zero/positive and non-LOD sampler state. Used only for XIII-2,
// whose renderer applies a negative bias to nearly every multi-level texture
std::uint32_t ClampNegativeMipLodBias(std::uint32_t state,
                                      std::uint32_t value, float minimumBias);

} // namespace novafix::display::texture_policy
