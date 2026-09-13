#pragma once

#include <cstdint>
#include <optional>

namespace novafix::game::xiii2_shadow_atlas_policy {

struct Layout {
    std::uint32_t cascadeWidth{};
    std::uint32_t cascadeHeight{};
    std::uint32_t atlasWidth{};
    std::uint32_t atlasHeight{};
};

struct Region {
    std::uint32_t left{};
    std::uint32_t top{};
    std::uint32_t right{};
    std::uint32_t bottom{};

    bool operator==(const Region&) const = default;
};

bool IsValid(const Layout& layout);
std::optional<Region> CascadeRegion(const Layout& layout,
                                    std::uint32_t cascadeIndex);
std::uint32_t FarCascadeRenderResolution(const Layout& layout);
bool UsesReducedFarCascade(const Layout& layout);
std::optional<Region> CascadeRenderRegion(const Layout& layout,
                                          std::uint32_t cascadeIndex);
std::uint32_t ConvertClearColor(std::uint32_t engineColor);

} // namespace novafix::game::xiii2_shadow_atlas_policy
