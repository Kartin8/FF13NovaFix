#pragma once

#include <cstdint>

namespace novafix::display::texture_policy {

// Clamp only negative mip LOD bias. Leave zero/positive bias and unrelated sampler states alone.
// XIII-2 applies a negative bias to nearly every mipmapped texture.
std::uint32_t ClampNegativeMipLodBias(std::uint32_t state,
                                      std::uint32_t value, float minimumBias);

} // namespace novafix::display::texture_policy
