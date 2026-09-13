#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace novafix::compat {

inline constexpr std::wstring_view kPrimaryAddonDirectoryName = L"addons";
inline constexpr std::array<std::wstring_view, 5> kAddonDirectoryNames{
    kPrimaryAddonDirectoryName,
    L"extra",
    L"extras",
    L"mods",
    L"dll",
};

struct AddonDirectorySelection {
    std::size_t primaryIndex{};
    bool createPrimary{};
};

constexpr AddonDirectorySelection ResolveAddonDirectorySelection(
    bool reuseExisting,
    const std::array<bool, kAddonDirectoryNames.size()>& existing) {
    if (reuseExisting) {
        for (std::size_t index = 0; index < existing.size(); ++index) {
            if (existing[index]) return {index, false};
        }
    }
    return {0u, !existing[0]};
}

} // namespace novafix::compat
