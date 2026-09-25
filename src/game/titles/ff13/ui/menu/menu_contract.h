#pragma once

#include <cstddef>
#include <cstdint>

namespace novafix::game::ff13_ui::menu_contract {

inline constexpr std::uint32_t kMissionListProductVtableRva =
    0x1152578u - 0x400000u;
inline constexpr std::uint32_t kMissionListVtableRva =
    0x1152708u - 0x400000u;
inline constexpr std::uint32_t kSettingsListVtableRva =
    0x114DAF0u - 0x400000u;
inline constexpr std::uint32_t kPartyStatusValueVtableRva =
    0x1148098u - 0x400000u;
inline constexpr std::uint32_t kBackdropVtableRva =
    0x119F880u - 0x400000u;
inline constexpr std::size_t kNodeParentOffset = 0x04u;
inline constexpr std::size_t kNodeFirstChildOffset = 0x08u;
inline constexpr std::size_t kNodeNextSiblingOffset = 0x14u;
inline constexpr std::size_t kNodeDirtyFlagsOffset = 0xCCu;
inline constexpr std::size_t kMissionListOffset = 0x70u;
inline constexpr std::size_t kMissionListWidthOffset = 0x1170u;
inline constexpr int kMissionCollapsedWidth = 510;
inline constexpr int kMissionExpandedWidth = 1280;

} // namespace novafix::game::ff13_ui::menu_contract
