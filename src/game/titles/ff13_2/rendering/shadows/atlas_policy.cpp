#include "game/titles/ff13_2/rendering/shadows/atlas_policy.h"

#include <limits>

namespace novafix::game::xiii2_shadow_atlas_policy {

bool IsValid(const Layout& layout) {
    if (layout.cascadeWidth == 0u || layout.cascadeHeight == 0u ||
        layout.atlasWidth != layout.cascadeWidth) {
        return false;
    }
    const std::uint64_t expectedAtlasHeight =
        static_cast<std::uint64_t>(layout.cascadeHeight) * 2u;
    return expectedAtlasHeight <= std::numeric_limits<std::uint32_t>::max() &&
           layout.atlasHeight ==
               static_cast<std::uint32_t>(expectedAtlasHeight) &&
           layout.atlasWidth <=
               static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
           layout.atlasHeight <=
               static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
}

std::optional<Region> CascadeRegion(const Layout& layout,
                                    std::uint32_t cascadeIndex) {
    if (!IsValid(layout) || cascadeIndex >= 2u) return std::nullopt;
    const std::uint32_t top = layout.cascadeHeight * cascadeIndex;
    return Region{0u, top, layout.cascadeWidth, top + layout.cascadeHeight};
}

std::uint32_t FarCascadeRenderResolution(const Layout& layout) {
    if (!IsValid(layout)) return 0u;
    return layout.cascadeWidth == 8192u &&
                   layout.cascadeHeight == 8192u
        ? 4096u
        : layout.cascadeWidth;
}

bool UsesReducedFarCascade(const Layout& layout) {
    const std::uint32_t resolution = FarCascadeRenderResolution(layout);
    return resolution != 0u && resolution < layout.cascadeWidth;
}

std::optional<Region> CascadeRenderRegion(const Layout& layout,
                                          std::uint32_t cascadeIndex) {
    if (!IsValid(layout) || cascadeIndex >= 2u) return std::nullopt;
    if (cascadeIndex == 0u || !UsesReducedFarCascade(layout)) {
        return CascadeRegion(layout, cascadeIndex);
    }
    const std::uint32_t resolution = FarCascadeRenderResolution(layout);
    const std::uint32_t inset =
        (layout.cascadeWidth - resolution) / 2u;
    const std::uint32_t top = layout.cascadeHeight + inset;
    return Region{inset, top, inset + resolution, top + resolution};
}

std::uint32_t ConvertClearColor(std::uint32_t engineColor) {
    return (engineColor & 0xFF00FF00u) |
           ((engineColor & 0x000000FFu) << 16u) |
           ((engineColor & 0x00FF0000u) >> 16u);
}

} // namespace novafix::game::xiii2_shadow_atlas_policy
