#pragma once

#include "display/aspect_ratio.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace novafix::game::ff13_ui {

enum class ColumnSide : int { Left = -1, Center = 0, Right = 1 };
enum class ColumnSelector { Field, Root, RootChildContaining, RootChildResource };

struct ColumnRule {
    ColumnSelector selector{};
    std::size_t value{};
    ColumnSide side{};
};
struct MenuColumnProfile {
    std::uint32_t vtableRva{};
    std::array<ColumnRule, 4> rules{};
};

inline constexpr std::array<MenuColumnProfile, 15> kMenuColumnProfiles{{
    {0x1153D90u - 0x400000u, {{{ColumnSelector::Field, 0x80, ColumnSide::Left},
                              {ColumnSelector::Field, 0x90, ColumnSide::Left}}}},
    {0x1153920u - 0x400000u, {{{ColumnSelector::Root, 0, ColumnSide::Right}}}},
    {0x11549A8u - 0x400000u, {{{ColumnSelector::RootChildContaining, 0x80, ColumnSide::Left}}}},
    {0x1152578u - 0x400000u, {{{ColumnSelector::Field, 0x70, ColumnSide::Left}}}},
    {0x11544F0u - 0x400000u, {{{ColumnSelector::Field, 0xF8, ColumnSide::Left},
                              {ColumnSelector::Field, 0xFC, ColumnSide::Right}}}},
    {0x114AF78u - 0x400000u, {{{ColumnSelector::Field, 0x70, ColumnSide::Left},
                              {ColumnSelector::Field, 0x74, ColumnSide::Right}}}},
    {0x11509E8u - 0x400000u, {{{ColumnSelector::Field, 0x94, ColumnSide::Left},
                              {ColumnSelector::Field, 0x98, ColumnSide::Right}}}},
    {0x114FCB0u - 0x400000u, {{{ColumnSelector::Field, 0x7C, ColumnSide::Left}}}},
    {0x11515C0u - 0x400000u, {{{ColumnSelector::Field, 0x70, ColumnSide::Left},
                              {ColumnSelector::Field, 0x74, ColumnSide::Right}}}},
    {0x114BA60u - 0x400000u, {{{ColumnSelector::Field, 0x7C, ColumnSide::Left}}}},
    {0x114B618u - 0x400000u, {{{ColumnSelector::Root, 0, ColumnSide::Right}}}},
    {0x114BFD0u - 0x400000u, {{{ColumnSelector::Root, 0, ColumnSide::Left}}}},
    {0x114C158u - 0x400000u, {{{ColumnSelector::Root, 0, ColumnSide::Left}}}},
    {0x114C2D8u - 0x400000u, {{{ColumnSelector::Root, 0, ColumnSide::Left}}}},
    {0x114E730u - 0x400000u, {{{ColumnSelector::Field, 4405u * 4u, ColumnSide::Left},
                              {ColumnSelector::Field, 4418u * 4u, ColumnSide::Left},
                              {ColumnSelector::Field, 4402u * 4u, ColumnSide::Left},
                              {ColumnSelector::RootChildResource, 740, ColumnSide::Left}}}},
}};

inline const MenuColumnProfile* FindMenuColumnProfile(std::uintptr_t rva) {
    for (const auto& profile : kMenuColumnProfiles)
        if (profile.vtableRva == rva) return &profile;
    return nullptr;
}

inline float HorizontalColumnOffset(const display::AspectRatio& aspect,
                                    ColumnSide side) {
    if (!aspect.valid || side == ColumnSide::Center ||
        !std::isfinite(aspect.safeScaleX) ||
        aspect.safeScaleX <= 0.0f || aspect.safeScaleX >= 1.0f) return 0.0f;
    const float offset = 640.0f * (1.0f / aspect.safeScaleX - 1.0f);
    return std::isfinite(offset) ? offset * static_cast<int>(side) : 0.0f;
}

template <typename Reader>
ColumnSide NodeColumnSide(const MenuColumnProfile& profile, void* product,
                          void* node, const Reader& reader) {
    void* root = reader.ProductNode(product, 0x34);
    if (!root || !node) return ColumnSide::Center;
    for (const auto& rule : profile.rules) {
        if (rule.side == ColumnSide::Center) continue;
        void* target = nullptr;
        switch (rule.selector) {
        case ColumnSelector::Field:
            target = reader.ProductNode(product, rule.value);
            if (target && reader.Parent(target) != root) target = nullptr;
            break;
        case ColumnSelector::Root:
            target = root;
            break;
        case ColumnSelector::RootChildContaining:
            target = reader.ProductNode(product, rule.value);
            for (unsigned depth = 0; target && target != root && depth != 64; ++depth) {
                void* parent = reader.Parent(target);
                if (parent == root) break;
                target = parent;
            }
            if (!target || target == root || reader.Parent(target) != root)
                target = nullptr;
            break;
        case ColumnSelector::RootChildResource:
            if (reader.Parent(node) == root &&
                reader.ResourceId(node) == static_cast<int>(rule.value))
                target = node;
            break;
        }
        if (target == node) return rule.side;
    }
    return ColumnSide::Center;
}

template <typename Reader>
ColumnSide InheritedColumnSide(const MenuColumnProfile& profile, void* product,
                               void* node, const Reader& reader) {
    for (unsigned depth = 0; node && depth != 64; ++depth) {
        const auto side = NodeColumnSide(profile, product, node, reader);
        if (side != ColumnSide::Center) return side;
        node = reader.Parent(node);
    }
    return ColumnSide::Center;
}

}
