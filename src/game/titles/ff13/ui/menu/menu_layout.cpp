#include "game/titles/ff13/ui/menu/menu_layout.h"

#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "game/titles/ff13/ui/battle_header_layout.h"
#include "game/titles/ff13/ui/gestalt_layout.h"
#include "game/titles/ff13/ui/menu/menu_chrome.h"
#include "game/titles/ff13/ui/menu/menu_column_layout.h"
#include "game/titles/ff13/ui/menu/menu_contract.h"
#include "game/titles/ff13/ui/minimap_tilt.h"
#include "patch/memory_access.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"
#include "settings/graphics_settings.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::ff13_ui::menu_layout {
namespace {

using menu_contract::kMissionListProductVtableRva;
using menu_contract::kMissionListVtableRva;
using menu_contract::kPartyStatusValueVtableRva;
using menu_contract::kBackdropVtableRva;
using menu_contract::kNodeParentOffset;
using menu_contract::kNodeFirstChildOffset;
using menu_contract::kNodeNextSiblingOffset;
using menu_contract::kNodeDirtyFlagsOffset;
using menu_contract::kMissionListOffset;
using menu_contract::kMissionListWidthOffset;
using menu_contract::kMissionCollapsedWidth;
using menu_contract::kMissionExpandedWidth;

constexpr std::uint32_t kMinimapProductVtableRva =
    0x11569D0u - 0x400000u;
constexpr std::uint32_t kMainMenuProductVtableRva =
    0x11549A8u - 0x400000u;
constexpr std::uint32_t kFieldMapProductVtableRva =
    0x11550D0u - 0x400000u;
constexpr std::uint32_t kFieldMapLegendVtableRva =
    0x119ED78u - 0x400000u;
constexpr std::uint32_t kMissionRowVtableRva =
    0x11528A8u - 0x400000u;
constexpr std::uint32_t kMissionScrollbarVtableRva =
    0x119DE00u - 0x400000u;
constexpr std::uint32_t kDatalogArticleProductVtableRva =
    0x11990E8u - 0x400000u;
constexpr std::uint32_t kSettingsProductVtableRva =
    0x114D4D0u - 0x400000u;
constexpr std::uint32_t kSettingsChoiceProductVtableRva =
    0x114D000u - 0x400000u;
constexpr std::uint32_t kSettingsLabelVtableRva =
    0x119C8D0u - 0x400000u;
constexpr std::uint32_t kSettingsHeaderTextVtableRva =
    0x119C2E8u - 0x400000u;
constexpr std::uint32_t kPartyStatusGroupVtableRva =
    0x119C470u - 0x400000u;
constexpr std::uint32_t kPartyStatusBackingVtableRva =
    0x119FBB0u - 0x400000u;
constexpr std::uint32_t kTitleLogoVtableRva = 0x11997B0u - 0x400000u;
constexpr std::uint32_t kTitleVisualRootVtableRva = 0x119C470u - 0x400000u;
constexpr std::uint32_t kProductUpdateDispatchRva = 0x8EB0BBu - 0x400000u;
constexpr std::uint32_t kGuiManagerPointerRva = 0x2826A4Cu - 0x400000u;
constexpr std::uint32_t kNodeTransformRva = 0x8D6990u - 0x400000u;
constexpr std::uint32_t kNodeTransformCallRva = 0x8D86E3u - 0x400000u;
constexpr std::size_t kNodeWorldYOffset = 0x4Cu;
constexpr std::size_t kPartyStatusRootOffset = 0x90u;
constexpr std::size_t kFieldMapLegendOffset = 0x138u;
constexpr std::size_t kMissionScrollbarOffset = 0x9Cu;
constexpr std::size_t kResourceIdOffset = 0xC0u;
constexpr std::size_t kNodeOwnerOffset = 0xC8u;
constexpr std::array<int, 3> kPartyStatusBackingIds{{902, 903, 906}};
constexpr std::array<std::byte, 5> kNodeTransformCall{
    std::byte{0xE8}, std::byte{0xA8}, std::byte{0xE2}, std::byte{0xFF},
    std::byte{0xFF}};

using MainMenuUpdateFn = int (__thiscall*)(void*, float);
using NodeTransformFn = void* (__thiscall*)(void*, float);

NodeTransformFn g_nodeTransform{};
void* g_backdropVtable{};
std::uintptr_t g_imageBase{};
void** g_guiManagerPointer{};

struct MenuColumnUpdateContext {
    void* product{};
    const ff13_ui::MenuColumnProfile* profile{};
    display::AspectRatio aspect{};
    void* titleLogoModel{};
    void* partyStatusRoot{};
    float partyStatusBottomOffset{};
    std::array<void*, 3> partyStatusBackings{};
    float partyWidthScale{1.0f};
    void* fieldMapBacking{};
    std::array<void*, 2> fieldMapLeftGroups{};
    float fieldMapWidthScale{1.0f};
    float fieldMapHeightScale{1.0f};
    float fieldMapLeftOffset{};
    void* missionList{};
    void* missionHeader{};
    void* missionScrollbar{};
    std::array<void*, 4> missionHeaderTexts{};
    float missionGutter{};
    float missionExpansion{};
    float articleGutter{};
    float settingsGutter{};
    float settingsChoiceGutter{};
};
thread_local MenuColumnUpdateContext g_menuColumnUpdate{};
struct MissionListCache {
    void* product{};
    void* root{};
    void* list{};
    void* header{};
    void* scrollbar{};
    std::array<void*, 4> labels{};
};
thread_local MissionListCache g_missionListCache{};
template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

enum class PanelEdge {
    None, Both, LeftFill, RightFill, LeftCap, RightCap
};

PanelEdge ArticlePanelRole(int resourceId) {
    // Article templates share these strip IDs across Datalog categories.
    switch (resourceId) {
    case 929: case 930: case 932:
    case 1266: case 1267: case 1282: case 1286: case 1288: case 1289:
    case 1293: case 1297: case 1302:
        return PanelEdge::Both;
    case 1272: case 1273: case 1276:
        return PanelEdge::LeftFill;
    case 1280:
        return PanelEdge::RightFill;
    case 1269: case 1271: case 1274: case 1275:
    case 1292: case 1295: case 1303:
        return PanelEdge::LeftCap;
    case 1304: case 1306: case 1307:
        return PanelEdge::RightCap;
    default:
        return PanelEdge::None;
    }
}

PanelEdge SettingsPanelRole(int resourceId) {
    switch (resourceId) {
    case 929: case 930: case 932:
    case 883: case 884: case 897:
        return PanelEdge::Both;
    case 888: case 889: case 892:
        return PanelEdge::LeftFill;
    case 885: case 887: case 890: case 891:
        return PanelEdge::LeftCap;
    default:
        return PanelEdge::None;
    }
}

bool SettingsChoiceBacking(int resourceId) {
    return resourceId == 1420 || resourceId == 1421 || resourceId == 1422;
}

void ExtendPanelNode(void* node, PanelEdge edge, float gutter) {
    if (edge == PanelEdge::None || gutter <= 0.0f) return;
    float& x = Field<float>(node, 0x48u);
    if (edge == PanelEdge::LeftCap) {
        x -= gutter;
        return;
    }
    if (edge == PanelEdge::RightCap) {
        x += gutter;
        return;
    }
    const float width = Field<float>(node, 0x18u);
    if (!std::isfinite(width) || width <= 0.0f) return;
    const float extension = edge == PanelEdge::Both
        ? 2.0f * gutter : gutter;
    const float scale = (width + extension) / width;
    if (!std::isfinite(scale)) return;
    for (const std::size_t index : {0u, 4u, 8u}) {
        Field<float>(node, 0x18u + index * sizeof(float)) *= scale;
    }
    if (edge != PanelEdge::RightFill) x -= gutter;
}

struct NativeColumnReader {
    void* ProductNode(void* product, std::size_t offset) const {
        return Field<void*>(product, offset);
    }
    void* Parent(void* node) const { return Field<void*>(node, kNodeParentOffset); }
    int ResourceId(void* node) const { return Field<int>(node, 0xC0u); }
};

const ff13_ui::MenuColumnProfile* ColumnProfile(void* product) {
    if (!product || g_imageBase == 0u) return nullptr;
    const auto vtable = reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u));
    return vtable >= g_imageBase
        ? ff13_ui::FindMenuColumnProfile(vtable - g_imageBase) : nullptr;
}

bool BelongsToProduct(void* node, void* product) {
    if (!product) return false;
    void* root = Field<void*>(product, 0x34u);
    for (unsigned depth = 0; node && depth != 64; ++depth) {
        if (node == root) return true;
        node = Field<void*>(node, kNodeParentOffset);
    }
    return false;
}

void CapturePartyStatus(void* product, MenuColumnUpdateContext& context) {
    if (!product || !context.aspect.valid ||
        !context.aspect.correctionRequired || g_imageBase == 0u ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u)) !=
            g_imageBase + kMainMenuProductVtableRva) return;

    void* root = Field<void*>(product, 0x34u);
    void* statusRoot = Field<void*>(product, kPartyStatusRootOffset);
    if (!root || !statusRoot ||
        !patch::memory::IsReadable(statusRoot, kNodeNextSiblingOffset +
                                       sizeof(void*)) ||
        Field<void*>(statusRoot, kNodeParentOffset) != root) return;

    if (context.aspect.safeScaleY < 1.0f) {
        const float offset = 360.0f *
            (1.0f / context.aspect.safeScaleY - 1.0f);
        if (std::isfinite(offset)) {
            context.partyStatusRoot = statusRoot;
            context.partyStatusBottomOffset = offset;
        }
    }
    if (context.aspect.safeScaleX >= 1.0f) return;

    void* statusGroup = nullptr;
    void* child = Field<void*>(statusRoot, kNodeFirstChildOffset);
    for (unsigned index = 0; child && index < 8; ++index) {
        if (!patch::memory::IsReadable(
                child, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
            Field<void*>(child, kNodeParentOffset) != statusRoot) return;
        if (Field<int>(child, kResourceIdOffset) == 900 &&
            reinterpret_cast<std::uintptr_t>(Field<void*>(child, 0u)) ==
                g_imageBase + kPartyStatusGroupVtableRva) {
            statusGroup = child;
            break;
        }
        child = Field<void*>(child, kNodeNextSiblingOffset);
    }
    if (!statusGroup) return;

    std::array<void*, 3> backings{};
    child = Field<void*>(statusGroup, kNodeFirstChildOffset);
    for (std::size_t index = 0; index < backings.size(); ++index) {
        if (!child || !patch::memory::IsWritable(
                child, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
            Field<void*>(child, kNodeParentOffset) != statusGroup ||
            Field<int>(child, kResourceIdOffset) !=
                kPartyStatusBackingIds[index] ||
            reinterpret_cast<std::uintptr_t>(Field<void*>(child, 0u)) !=
                g_imageBase + kPartyStatusBackingVtableRva) return;
        backings[index] = child;
        child = Field<void*>(child, kNodeNextSiblingOffset);
    }

    context.partyStatusBackings = backings;
    context.partyWidthScale = 1.0f / context.aspect.safeScaleX;
}

void CaptureFieldMapLayout(void* product, MenuColumnUpdateContext& context) {
    if (!product || !context.aspect.valid ||
        !context.aspect.correctionRequired || g_imageBase == 0u ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u)) !=
            g_imageBase + kFieldMapProductVtableRva) return;

    void* root = Field<void*>(product, 0x34u);
    if (!root || !patch::memory::IsReadable(
            root, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(root, 0u)) !=
            g_imageBase + kPartyStatusGroupVtableRva) return;

    void* background = nullptr;
    void* sidebar = nullptr;
    void* group = Field<void*>(root, kNodeFirstChildOffset);
    for (unsigned index = 0; group && index < 32; ++index) {
        if (!patch::memory::IsReadable(
                group, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
            Field<void*>(group, kNodeParentOffset) != root) return;
        const int groupId = Field<int>(group, kResourceIdOffset);
        if (groupId == 1054 || groupId == 1380) {
            void* backing = Field<void*>(group, kNodeFirstChildOffset);
            const int expectedId = groupId == 1054 ? 1055 : 1056;
            if (!backing || !patch::memory::IsWritable(
                    backing, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
                Field<void*>(backing, kNodeParentOffset) != group ||
                Field<int>(backing, kResourceIdOffset) != expectedId ||
                reinterpret_cast<std::uintptr_t>(Field<void*>(backing, 0u)) !=
                    g_imageBase + kPartyStatusBackingVtableRva) return;
            if (groupId == 1054) background = backing;
            else sidebar = group;
        }
        group = Field<void*>(group, kNodeNextSiblingOffset);
    }
    if (!background || !sidebar) return;
    context.fieldMapBacking = background;
    context.fieldMapWidthScale = 1.0f / context.aspect.safeScaleX;
    context.fieldMapHeightScale = 1.0f / context.aspect.safeScaleY;
    if (context.aspect.safeScaleX >= 1.0f) return;

    void* legend = Field<void*>(product, kFieldMapLegendOffset);
    if (!legend || !patch::memory::IsReadable(
            legend, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
        Field<void*>(legend, kNodeParentOffset) != root ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(legend, 0u)) !=
            g_imageBase + kFieldMapLegendVtableRva) return;
    context.fieldMapLeftGroups = {sidebar, legend};
    context.fieldMapLeftOffset = ff13_ui::HorizontalColumnOffset(
        context.aspect, ff13_ui::ColumnSide::Left);
}

void CaptureMissionListLayout(void* product, MenuColumnUpdateContext& context) {
    if (!product || !context.aspect.valid ||
        context.aspect.safeScaleX >= 1.0f || g_imageBase == 0u ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u)) !=
            g_imageBase + kMissionListProductVtableRva) return;

    void* root = Field<void*>(product, 0x34u);
    void* list = Field<void*>(product, kMissionListOffset);
    if (!root || !list) return;
    const bool cachedRootAndList = g_missionListCache.product == product &&
        g_missionListCache.root == root &&
        g_missionListCache.list == list;
    if (!cachedRootAndList &&
        (!patch::memory::IsReadable(
            root, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
         !patch::memory::IsReadable(
            list, kMissionListWidthOffset + sizeof(int)))) return;
    if (
        reinterpret_cast<std::uintptr_t>(Field<void*>(root, 0u)) !=
            g_imageBase + kMissionRowVtableRva ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(list, 0u)) !=
            g_imageBase + kMissionListVtableRva ||
        Field<void*>(list, kNodeParentOffset) != root) return;

    void* header = Field<void*>(root, kNodeFirstChildOffset);
    if (!header) return;
    if ((!cachedRootAndList || g_missionListCache.header != header) &&
        !patch::memory::IsReadable(
            header, kNodeDirtyFlagsOffset + sizeof(std::uint32_t))) return;
    if (
        Field<void*>(header, kNodeParentOffset) != root ||
        Field<int>(header, kResourceIdOffset) != 927 ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(header, 0u)) !=
            g_imageBase + kPartyStatusGroupVtableRva) return;

    const bool cacheValid = g_missionListCache.product == product &&
        g_missionListCache.root == root &&
        g_missionListCache.list == list &&
        g_missionListCache.header == header &&
        g_missionListCache.scrollbar ==
            Field<void*>(product, kMissionScrollbarOffset);
    if (!cacheValid) {
        std::array<void*, 4> labels{};
        std::size_t labelCount = 0;
        void* child = Field<void*>(header, kNodeFirstChildOffset);
        for (unsigned index = 0; child && index < 16; ++index) {
            if (!patch::memory::IsReadable(
                    child, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
                Field<void*>(child, kNodeParentOffset) != header) return;
            if (reinterpret_cast<std::uintptr_t>(Field<void*>(child, 0u)) ==
                    g_imageBase + kSettingsHeaderTextVtableRva &&
                labelCount < labels.size()) {
                labels[labelCount++] = child;
            }
            child = Field<void*>(child, kNodeNextSiblingOffset);
        }
        if (labelCount != labels.size()) return;

        void* scrollbar = Field<void*>(product, kMissionScrollbarOffset);
        if (!scrollbar || !patch::memory::IsReadable(
                scrollbar, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
            Field<void*>(scrollbar, kNodeParentOffset) != list ||
            reinterpret_cast<std::uintptr_t>(Field<void*>(scrollbar, 0u)) !=
                g_imageBase + kMissionScrollbarVtableRva) return;
        g_missionListCache = {product, root, list, header, scrollbar, labels};
    }

    const int width = Field<int>(list, kMissionListWidthOffset);
    if (width < kMissionCollapsedWidth ||
        width > kMissionExpandedWidth) return;

    context.missionList = list;
    context.missionHeader = header;
    context.missionHeaderTexts = g_missionListCache.labels;
    context.missionGutter = ff13_ui::HorizontalColumnOffset(
        context.aspect, ff13_ui::ColumnSide::Right);
    context.missionExpansion =
        static_cast<float>(width - kMissionCollapsedWidth) /
        static_cast<float>(kMissionExpandedWidth - kMissionCollapsedWidth);

    context.missionScrollbar = g_missionListCache.scrollbar;
}

int __fastcall MenuProductUpdateHook(void* product, void*, float delta) {
    struct RestoreContext {
        MenuColumnUpdateContext previous{g_menuColumnUpdate};
        ~RestoreContext() { g_menuColumnUpdate = previous; }
    } restore;
    const auto aspect = display::CurrentAspectRatio();
    const battle_header_layout::ScopedProduct battleLayout(
        product, aspect, g_imageBase);
    const gestalt_layout::ScopedProduct gestaltLayout(
        product, aspect, g_imageBase);
    g_menuColumnUpdate = {product, ColumnProfile(product), aspect};
    CapturePartyStatus(product, g_menuColumnUpdate);
    CaptureFieldMapLayout(product, g_menuColumnUpdate);
    CaptureMissionListLayout(product, g_menuColumnUpdate);
    if (product && aspect.valid && aspect.safeScaleX < 1.0f &&
        g_imageBase != 0u &&
        reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u)) ==
            g_imageBase + kDatalogArticleProductVtableRva) {
        g_menuColumnUpdate.articleGutter =
            ff13_ui::HorizontalColumnOffset(aspect,
                ff13_ui::ColumnSide::Right);
    }
    if (product && aspect.valid && aspect.safeScaleX < 1.0f &&
        g_imageBase != 0u) {
        const auto vtable = reinterpret_cast<std::uintptr_t>(
            Field<void*>(product, 0u));
        const float gutter = ff13_ui::HorizontalColumnOffset(
            aspect, ff13_ui::ColumnSide::Right);
        if (vtable == g_imageBase + kSettingsProductVtableRva) {
            g_menuColumnUpdate.settingsGutter = gutter;
        } else if (vtable ==
                   g_imageBase + kSettingsChoiceProductVtableRva) {
            g_menuColumnUpdate.settingsChoiceGutter = gutter;
        }
    }
    if (product && aspect.valid && aspect.correctionRequired &&
        g_imageBase != 0u &&
        reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u)) ==
            g_imageBase + kTitleLogoVtableRva) {
        const int mode = Field<int>(product, 0x184u);
        void* root = Field<void*>(product, 0x34u);
        if ((mode == 4 || mode == 5) && root &&
            reinterpret_cast<std::uintptr_t>(Field<void*>(root, 0u)) ==
                g_imageBase + kTitleVisualRootVtableRva) {
            // The title art is the root's 3D child; menu rows are a separate
            // product. A69B40 publishes this child's world matrix to VFX.
            void* model = Field<void*>(root, kNodeFirstChildOffset);
            if (model && Field<void*>(model, 0u) == g_backdropVtable &&
                Field<void*>(model, kNodeParentOffset) == root) {
                g_menuColumnUpdate.titleLogoModel = model;
            }
        }
    }
    const auto update = Field<MainMenuUpdateFn>(Field<void*>(product, 0u), 0xF8u);
    const bool roundMinimap = product && g_imageBase != 0u &&
        reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u)) ==
            g_imageBase + kMinimapProductVtableRva &&
        settings::RoundMinimapEnabled();
    const ScopedMinimapTilt tilt(product, roundMinimap);
    return update(product, delta);
}

float NodeColumnOffset(void* node) {
    const auto& context = g_menuColumnUpdate;
    if (!node || !context.profile || context.aspect.safeScaleX >= 1.0f)
        return 0.0f;
    return ff13_ui::HorizontalColumnOffset(context.aspect,
        ff13_ui::NodeColumnSide(*context.profile, context.product, node,
                                NativeColumnReader{}));
}

void CorrectLocalCursorImpl(void* widget, float* position) {
    if (!widget || !position || position[3] == 0.0f) return;
    const auto aspect = display::CurrentAspectRatio();
    if (!aspect.valid || aspect.safeScaleX >= 1.0f) return;
    void* product = Field<void*>(widget, 0xC8u);
    if (!ColumnProfile(product) || !BelongsToProduct(widget, product)) {
        product = g_menuColumnUpdate.product;
        if (!ColumnProfile(product) || !BelongsToProduct(widget, product)) {
            void* manager = g_guiManagerPointer ? *g_guiManagerPointer : nullptr;
            product = manager ? Field<void*>(manager, 724u) : nullptr;
        }
    }
    const auto* profile = ColumnProfile(product);
    if (!profile || !BelongsToProduct(widget, product)) return;
    position[0] += ff13_ui::HorizontalColumnOffset(aspect,
        ff13_ui::InheritedColumnSide(*profile, product, widget,
                                     NativeColumnReader{}));
}

void* __fastcall NodeTransformHook(void* node, void*, float delta) {
    const auto& chrome = menu_chrome::Current();
    const bool battleNode = battle_header_layout::Affects(node);
    const bool gestaltNode = gestalt_layout::Affects(node);
    const bool partyStatusRoot = node &&
        node == g_menuColumnUpdate.partyStatusRoot;
    const float offset = node == chrome.header
        ? -chrome.offset
        : node == chrome.footer ? chrome.offset
        : partyStatusRoot ? g_menuColumnUpdate.partyStatusBottomOffset : 0.0f;
    const bool partyStatusBacking = node && std::find(
        g_menuColumnUpdate.partyStatusBackings.begin(),
        g_menuColumnUpdate.partyStatusBackings.end(), node) !=
        g_menuColumnUpdate.partyStatusBackings.end();
    const bool fieldMapBacking = node &&
        node == g_menuColumnUpdate.fieldMapBacking;
    const bool fieldMapLeftGroup = node && std::find(
        g_menuColumnUpdate.fieldMapLeftGroups.begin(),
        g_menuColumnUpdate.fieldMapLeftGroups.end(), node) !=
        g_menuColumnUpdate.fieldMapLeftGroups.end();
    const bool missionNode = node && g_menuColumnUpdate.missionList;
    void* const missionParent = missionNode
        ? Field<void*>(node, kNodeParentOffset) : nullptr;
    const auto missionVtable = missionNode
        ? reinterpret_cast<std::uintptr_t>(Field<void*>(node, 0u)) : 0u;
    const int missionResourceId = missionNode
        ? Field<int>(node, kResourceIdOffset) : -1;
    const bool missionHeaderPanel = missionNode && missionParent ==
            g_menuColumnUpdate.missionHeader &&
        missionVtable ==
            g_imageBase + kPartyStatusBackingVtableRva &&
        (missionResourceId == 929 || missionResourceId == 930 ||
         missionResourceId == 932);
    const bool missionRowBacking = missionNode && missionParent &&
        g_menuColumnUpdate.missionExpansion > 0.0f &&
        missionVtable == g_imageBase + kPartyStatusBackingVtableRva &&
        (missionResourceId == 883 || missionResourceId == 884 ||
         missionResourceId == 897) &&
        Field<int>(missionParent, kResourceIdOffset) == 879;
    const bool missionRatingCandidate = missionNode &&
        missionVtable == g_imageBase + kMissionRowVtableRva;
    const bool missionRatingGroup = missionRatingCandidate &&
        missionParent &&
        Field<int>(missionParent, kResourceIdOffset) == 878;
    const bool widen = node && (node == chrome.headerBacking ||
        node == chrome.footerBacking || partyStatusBacking ||
        fieldMapBacking);
    const bool backdrop = node && node == chrome.backdrop;
    const bool titleLogoModel = node &&
        node == g_menuColumnUpdate.titleLogoModel;
    const PanelEdge articlePanel = node &&
        g_menuColumnUpdate.articleGutter > 0.0f &&
        Field<void*>(node, kNodeOwnerOffset) == g_menuColumnUpdate.product &&
        reinterpret_cast<std::uintptr_t>(Field<void*>(node, 0u)) ==
            g_imageBase + kPartyStatusBackingVtableRva
        ? ArticlePanelRole(Field<int>(node, kResourceIdOffset))
        : PanelEdge::None;
    const bool settingsNode = node &&
        g_menuColumnUpdate.settingsGutter > 0.0f &&
        Field<void*>(node, kNodeOwnerOffset) == g_menuColumnUpdate.product;
    const auto nodeVtable = settingsNode
        ? reinterpret_cast<std::uintptr_t>(Field<void*>(node, 0u)) : 0u;
    const int resourceId = settingsNode
        ? Field<int>(node, kResourceIdOffset) : -1;
    const PanelEdge settingsPanel = settingsNode &&
        nodeVtable == g_imageBase + kPartyStatusBackingVtableRva
        ? SettingsPanelRole(resourceId) : PanelEdge::None;
    const bool settingsChoicePanel = node &&
        g_menuColumnUpdate.settingsChoiceGutter > 0.0f &&
        Field<void*>(node, kNodeOwnerOffset) == g_menuColumnUpdate.product &&
        reinterpret_cast<std::uintptr_t>(Field<void*>(node, 0u)) ==
            g_imageBase + kPartyStatusBackingVtableRva &&
        SettingsChoiceBacking(Field<int>(node, kResourceIdOffset));
    float settingsTextOffset = 0.0f;
    if (settingsNode &&
        nodeVtable == g_imageBase + kSettingsLabelVtableRva &&
        resourceId == 898) {
        settingsTextOffset = -g_menuColumnUpdate.settingsGutter;
    } else if (settingsNode &&
               nodeVtable == g_imageBase + kPartyStatusValueVtableRva &&
               resourceId == -1) {
        settingsTextOffset = g_menuColumnUpdate.settingsGutter;
    } else if (settingsNode &&
               nodeVtable == g_imageBase + kSettingsHeaderTextVtableRva &&
               resourceId == -1) {
        settingsTextOffset = Field<float>(node, 0x48u) < 640.0f
            ? -g_menuColumnUpdate.settingsGutter
            : g_menuColumnUpdate.settingsGutter;
    }
    const float footerValueOffset = node &&
        (node == chrome.footerTime ||
         node == chrome.footerGil)
        ? chrome.footerValueOffset
        : 0.0f;
    const float footerGuideOffset = node == chrome.footerGuide
        ? chrome.footerGuideOffset
        : 0.0f;
    const auto headerText = std::find(
        g_menuColumnUpdate.missionHeaderTexts.begin(),
        g_menuColumnUpdate.missionHeaderTexts.end(), node);
    const float missionTextOffset = node && headerText !=
            g_menuColumnUpdate.missionHeaderTexts.end()
        ? (headerText - g_menuColumnUpdate.missionHeaderTexts.begin() < 2
            ? -g_menuColumnUpdate.missionGutter
            : g_menuColumnUpdate.missionGutter *
                g_menuColumnUpdate.missionExpansion)
        : missionRatingGroup
            ? 2.0f * g_menuColumnUpdate.missionGutter *
                g_menuColumnUpdate.missionExpansion
        : node == g_menuColumnUpdate.missionScrollbar
            ? 2.0f * g_menuColumnUpdate.missionGutter *
                g_menuColumnUpdate.missionExpansion
            : 0.0f;
    const float columnOffset = NodeColumnOffset(node) +
        footerValueOffset + footerGuideOffset + settingsTextOffset +
        (fieldMapLeftGroup ? g_menuColumnUpdate.fieldMapLeftOffset : 0.0f) +
        missionTextOffset;
    if ((offset == 0.0f && !battleNode && !gestaltNode && !widen && !backdrop && !titleLogoModel &&
         articlePanel == PanelEdge::None &&
         settingsPanel == PanelEdge::None &&
         !settingsChoicePanel &&
         !missionHeaderPanel && !missionRowBacking &&
         columnOffset == 0.0f) ||
        (Field<std::uint32_t>(node, kNodeDirtyFlagsOffset) & 1u) != 0u) {
        return g_nodeTransform(node, delta);
    }
    Field<std::uint32_t>(node, kNodeDirtyFlagsOffset) |= 4u;
    void* result = g_nodeTransform(node, delta);
    Field<float>(node, kNodeWorldYOffset) += offset;
    if (widen || backdrop) {
        const float widthScale = partyStatusBacking
            ? g_menuColumnUpdate.partyWidthScale
            : fieldMapBacking ? g_menuColumnUpdate.fieldMapWidthScale
            : chrome.widthScale;
        for (const std::size_t index : {0u, 4u, 8u}) {
            Field<float>(node, 0x18u + index * sizeof(float)) *=
                widthScale;
        }
        float& x = Field<float>(node, 0x48u);
        x = 640.0f + (x - 640.0f) * widthScale;
    }
    if (backdrop) {
        for (const std::size_t index : {1u, 5u, 9u}) {
            Field<float>(node, 0x18u + index * sizeof(float)) *=
                chrome.heightScale;
        }
        float& y = Field<float>(node, kNodeWorldYOffset);
        y = 360.0f + (y - 360.0f) * chrome.heightScale;
    }
    if (fieldMapBacking) {
        for (const std::size_t index : {1u, 5u, 9u}) {
            Field<float>(node, 0x18u + index * sizeof(float)) *=
                g_menuColumnUpdate.fieldMapHeightScale;
        }
        float& y = Field<float>(node, kNodeWorldYOffset);
        y = 360.0f + (y - 360.0f) *
            g_menuColumnUpdate.fieldMapHeightScale;
    }
    if (titleLogoModel) {
        for (const std::size_t index : {0u, 4u, 8u}) {
            Field<float>(node, 0x18u + index * sizeof(float)) *=
                g_menuColumnUpdate.aspect.safeScaleX;
        }
        for (const std::size_t index : {1u, 5u, 9u}) {
            Field<float>(node, 0x18u + index * sizeof(float)) *=
                g_menuColumnUpdate.aspect.safeScaleY;
        }
        float& x = Field<float>(node, 0x48u);
        float& y = Field<float>(node, kNodeWorldYOffset);
        x = 640.0f +
            (x - 640.0f) * g_menuColumnUpdate.aspect.safeScaleX;
        y = 360.0f +
            (y - 360.0f) * g_menuColumnUpdate.aspect.safeScaleY;
    }
    ExtendPanelNode(node, articlePanel,
                    g_menuColumnUpdate.articleGutter);
    ExtendPanelNode(node, settingsPanel,
                    g_menuColumnUpdate.settingsGutter);
    if (missionHeaderPanel) {
        ExtendPanelNode(node, PanelEdge::Both,
                        g_menuColumnUpdate.missionGutter);
    }
    if (missionRowBacking) {
        ExtendPanelNode(node, PanelEdge::RightFill,
            2.0f * g_menuColumnUpdate.missionGutter *
                g_menuColumnUpdate.missionExpansion);
    }
    if (settingsChoicePanel) {
        ExtendPanelNode(node, PanelEdge::Both,
                        g_menuColumnUpdate.settingsChoiceGutter);
    }
    Field<float>(node, 0x48u) += columnOffset;
    if (battleNode) battle_header_layout::Apply(node);
    if (gestaltNode) gestalt_layout::Apply(node);
    Field<std::uint32_t>(node, kNodeDirtyFlagsOffset) |= 8u;
    return result;
}

void* Target(const ImageView& image, std::uint32_t rva) {
    const auto range = image.At(rva, 1u);
    return range ? const_cast<std::byte*>(range->data()) : nullptr;
}

} // namespace

bool RegisterProductUpdatePatch(patch::Transaction& calls,
                                const ImageView&) {
    static_assert(sizeof(std::uintptr_t) == 4u);
    constexpr std::array<std::byte, 6> updateDispatch{
        std::byte{0x8B}, std::byte{0x90}, std::byte{0xF8},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    std::array<std::byte, 6> updateReplacement{};
    updateReplacement[0] = std::byte{0xBA};
    updateReplacement[5] = std::byte{0x90};
    const auto updateHook =
        reinterpret_cast<std::uintptr_t>(&MenuProductUpdateHook);
    std::memcpy(updateReplacement.data() + 1, &updateHook,
                sizeof(updateHook));
    return calls.Add(kProductUpdateDispatchRva, updateDispatch,
                     updateReplacement);
}

bool RegisterNodeTransformCall(patch::Transaction& calls,
                               const ImageView& image) {
    const auto replacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(Target(image, kNodeTransformCallRva)),
        reinterpret_cast<std::uintptr_t>(&NodeTransformHook));
    return replacement && calls.Add(kNodeTransformCallRva,
                                    kNodeTransformCall, *replacement);
}

void Prepare(const ImageView& image) {
    g_nodeTransform =
        reinterpret_cast<NodeTransformFn>(Target(image, kNodeTransformRva));
    g_backdropVtable = Target(image, kBackdropVtableRva);
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    g_guiManagerPointer =
        reinterpret_cast<void**>(Target(image, kGuiManagerPointerRva));
}

void CorrectLocalCursor(void* widget, float* position) {
    CorrectLocalCursorImpl(widget, position);
}

bool HasColumnProfile(void* product) {
    return ColumnProfile(product) != nullptr;
}

} // namespace novafix::game::ff13_ui::menu_layout
