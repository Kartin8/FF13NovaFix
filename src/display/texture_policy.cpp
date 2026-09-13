#include "display/texture_policy.h"

#include <cstring>

namespace novafix::display::texture_policy {
std::uint32_t ClampNegativeMipLodBias(std::uint32_t state,
                                      std::uint32_t value, float minimumBias) {
    constexpr std::uint32_t kMipMapLodBias = 8;
    if (state != kMipMapLodBias) return value;

    float bias{};
    static_assert(sizeof(bias) == sizeof(value));
    std::memcpy(&bias, &value, sizeof(bias));
    if (!(bias < 0.0f) || !(bias < minimumBias)) return value;

    std::uint32_t adjusted{};
    static_assert(sizeof(adjusted) == sizeof(minimumBias));
    std::memcpy(&adjusted, &minimumBias, sizeof(adjusted));
    return adjusted;
}

} // namespace novafix::display::texture_policy
