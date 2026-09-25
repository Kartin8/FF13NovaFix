#include "game/titles/ff13/ui/menu/menu_list_clip.h"

#include "display/aspect_runtime.h"
#include "display/scissor_transform.h"
#include "game/core/image_view.h"
#include "game/shared/ui/scissor_fix.h"
#include "game/titles/ff13/ui/menu/menu_column_layout.h"
#include "game/titles/ff13/ui/menu/menu_contract.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace novafix::game::ff13_ui::menu_list_clip {
namespace {

using menu_contract::kMissionListProductVtableRva;
using menu_contract::kMissionListVtableRva;
using menu_contract::kSettingsListVtableRva;
using menu_contract::kMissionListOffset;
using menu_contract::kMissionListWidthOffset;
using menu_contract::kMissionCollapsedWidth;
using menu_contract::kMissionExpandedWidth;

constexpr std::uint32_t kMissionListDrawRva =
    0x9BDA80u - 0x400000u;
constexpr std::uint32_t kMissionScissorSubmitCallRva =
    0x9BDAE8u - 0x400000u;
constexpr std::uint32_t kDatalogListVtableRva =
    0x114C4A8u - 0x400000u;
constexpr std::uint32_t kDatalogListDrawRva =
    0x9662F0u - 0x400000u;
constexpr std::uint32_t kEnemySubtypeListVtableRva =
    0x114C430u - 0x400000u;
constexpr std::uint32_t kEnemySubtypeListDrawRva =
    0x965690u - 0x400000u;
constexpr std::uint32_t kSettingsListDrawRva =
    0x972AB0u - 0x400000u;
constexpr std::uint32_t kNativeScissorSubmitRva =
    0xEABFE0u - 0x400000u;
constexpr std::array<std::uint32_t, 3> kListScissorSubmitCallRvas{{
    0xA68EBBu - 0x400000u,
    0xA68F11u - 0x400000u,
    kMissionScissorSubmitCallRva,
}};
constexpr std::size_t kDatalogListClipXOffset = 0x154u;
constexpr std::size_t kEnemySubtypeListClipXOffset = 0x2E8u;
constexpr std::size_t kSettingsListClipXOffset = 0x1C0u;
constexpr std::array<std::byte, 9> kDatalogListDrawPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x0C}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xF4}};
constexpr std::array<std::byte, 9> kSettingsListDrawPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x0C}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xF4}};
constexpr std::array<std::byte, 9> kMissionListDrawPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x28}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xD8}};

using GuiListDrawFn = void (__thiscall*)(void*, int);
using MissionListDrawFn = void* (__thiscall*)(void*, int);
using NativeScissorSubmitFn = void (__cdecl*)(int, int, int, int);

patch::Detour32 g_datalogListDrawDetour;
patch::Detour32 g_enemySubtypeListDrawDetour;
patch::Detour32 g_settingsListDrawDetour;
patch::Detour32 g_missionListDrawDetour;
GuiListDrawFn g_datalogListDraw{};
GuiListDrawFn g_enemySubtypeListDraw{};
GuiListDrawFn g_settingsListDraw{};
MissionListDrawFn g_missionListDraw{};
NativeScissorSubmitFn g_nativeScissorSubmit{};
thread_local bool g_listScissorPending{};
thread_local bool g_listScissorActive{};
thread_local bool g_missionScissorScope{};
thread_local bool g_missionNestedScissorPending{};
thread_local int g_missionNativeScissorLeft{};
thread_local int g_missionNativeScissorWidth{};
thread_local float g_missionScissorExpansion{};
thread_local display::ScissorRect g_listScissorRect{};
thread_local display::ScissorRect g_listScissorSource{};
std::uintptr_t g_imageBase{};

template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

void* Target(const ImageView& image, std::uint32_t rva) {
    const auto range = image.At(rva, 1u);
    return range ? const_cast<std::byte*>(range->data()) : nullptr;
}

bool HasListVtable(void* list, std::uint32_t vtableRva) {
    return list && g_imageBase != 0u &&
        patch::memory::IsReadable(list, sizeof(void*)) &&
        reinterpret_cast<std::uintptr_t>(Field<void*>(list, 0u)) ==
            g_imageBase + vtableRva;
}

bool ResolveListScissor(void* list, std::size_t clipXOffset,
                        bool extendToViewport,
                        const display::AspectRatio& aspect,
                        display::ScissorRect& output) {
    if (!aspect.valid || !patch::memory::IsReadable(
            list, clipXOffset + 3u * sizeof(int))) return false;
    const int width = Field<int>(list, clipXOffset + sizeof(int));
    const int height = Field<int>(list, clipXOffset + 2u * sizeof(int));
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return false;

    const auto* matrix = reinterpret_cast<const float*>(
        static_cast<const std::byte*>(list) + 0x18u);
    for (const std::size_t index : {0u, 1u, 4u, 5u, 12u, 13u}) {
        if (!std::isfinite(matrix[index])) return false;
    }
    constexpr float kMatrixTolerance = 0.0001f;
    if (std::abs(matrix[3]) > kMatrixTolerance ||
        std::abs(matrix[7]) > kMatrixTolerance ||
        std::abs(matrix[15] - 1.0f) > kMatrixTolerance) return false;
    const double left = Field<int>(list, clipXOffset);
    const double right = left + width;
    const auto point = [matrix](double x, double y) {
        return std::array<double, 2>{
            matrix[0] * x + matrix[4] * y + matrix[12],
            matrix[1] * x + matrix[5] * y + matrix[13]};
    };
    const std::array corners{
        point(left, 0.0), point(right, 0.0),
        point(left, height), point(right, height)};
    double minX = corners[0][0];
    double maxX = minX;
    double minY = corners[0][1];
    double maxY = minY;
    for (const auto& corner : corners) {
        minX = std::min(minX, corner[0]);
        maxX = std::max(maxX, corner[0]);
        minY = std::min(minY, corner[1]);
        maxY = std::max(maxY, corner[1]);
    }
    constexpr double limit = static_cast<double>(
        std::numeric_limits<std::int32_t>::max() / 2);
    if (!std::isfinite(minX) || !std::isfinite(maxX) ||
        !std::isfinite(minY) || !std::isfinite(maxY) ||
        std::max({std::abs(minX), std::abs(maxX),
                  std::abs(minY), std::abs(maxY)}) > limit) return false;
    display::ScissorRect authored{
        static_cast<std::int32_t>(std::floor(minX)),
        static_cast<std::int32_t>(std::floor(minY)),
        static_cast<std::int32_t>(std::ceil(maxX)),
        static_cast<std::int32_t>(std::ceil(maxY))};
    if (extendToViewport && aspect.safeScaleX < 1.0f) {
        const auto gutter = static_cast<std::int32_t>(std::ceil(
            ff13_ui::HorizontalColumnOffset(
                aspect, ff13_ui::ColumnSide::Right)));
        authored.left -= gutter;
        authored.right += gutter;
    }
    output = display::FitAuthoredScissorRectToViewport(authored, aspect);
    return output.right > output.left && output.bottom > output.top;
}

void DrawClippedList(void* list, int pass, GuiListDrawFn draw,
                     std::uint32_t vtableRva, std::size_t clipXOffset,
                     bool extendToViewport) {
    display::ScissorRect rect{};
    if (!HasListVtable(list, vtableRva) || !ResolveListScissor(
            list, clipXOffset, extendToViewport,
            display::CurrentAspectRatio(), rect)) {
        draw(list, pass);
        return;
    }
    const bool previousPending = g_listScissorPending;
    const bool previousActive = g_listScissorActive;
    const auto previousRect = g_listScissorRect;
    const auto previousSource = g_listScissorSource;
    struct Restore {
        bool pending;
        bool active;
        display::ScissorRect rect;
        display::ScissorRect source;
        ~Restore() {
            g_listScissorPending = pending;
            g_listScissorActive = active;
            g_listScissorRect = rect;
            g_listScissorSource = source;
        }
    } restore{previousPending, previousActive, previousRect, previousSource};
    g_listScissorRect = rect;
    g_listScissorPending = true;
    g_listScissorActive = false;
    draw(list, pass);
}

void __fastcall DatalogListDrawHook(void* list, void*, int pass) {
    DrawClippedList(list, pass, g_datalogListDraw,
                    kDatalogListVtableRva, kDatalogListClipXOffset, false);
}

void __fastcall EnemySubtypeListDrawHook(void* list, void*, int pass) {
    DrawClippedList(list, pass, g_enemySubtypeListDraw,
                    kEnemySubtypeListVtableRva,
                    kEnemySubtypeListClipXOffset, false);
}

void __fastcall SettingsListDrawHook(void* list, void*, int pass) {
    DrawClippedList(list, pass, g_settingsListDraw,
                    kSettingsListVtableRva, kSettingsListClipXOffset, true);
}

void* __fastcall MissionListDrawHook(void* product, void*, int pass) {
    const auto aspect = display::CurrentAspectRatio();
    if (!product || !aspect.valid || aspect.safeScaleX >= 1.0f ||
        !scissor_fix::IsActive() ||
        g_imageBase == 0u ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u)) !=
            g_imageBase + kMissionListProductVtableRva ||
        !patch::memory::IsReadable(
            product, kMissionListOffset + sizeof(void*))) {
        return g_missionListDraw(product, pass);
    }
    void* list = Field<void*>(product, kMissionListOffset);
    if (!list || !patch::memory::IsReadable(
            list, kMissionListWidthOffset + sizeof(int)) ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(list, 0u)) !=
            g_imageBase + kMissionListVtableRva) {
        return g_missionListDraw(product, pass);
    }

    const int width = Field<int>(list, kMissionListWidthOffset);
    if (width < kMissionCollapsedWidth ||
        width > kMissionExpandedWidth) {
        return g_missionListDraw(product, pass);
    }

    const float gutter = ff13_ui::HorizontalColumnOffset(
        aspect, ff13_ui::ColumnSide::Right);
    const float progress =
        static_cast<float>(width - kMissionCollapsedWidth) /
        static_cast<float>(kMissionExpandedWidth - kMissionCollapsedWidth);
    const display::ScissorRect authored{
        static_cast<std::int32_t>(std::floor(-gutter)), 0,
        static_cast<std::int32_t>(std::ceil(
            width + gutter * (2.0f * progress - 1.0f))), 720};
    display::ScissorRect rect =
        display::FitAuthoredScissorRectToViewport(authored, aspect);
    rect.top = 0;
    rect.bottom = static_cast<std::int32_t>(aspect.height);
    if (rect.right <= rect.left) return g_missionListDraw(product, pass);

    const bool previousPending = g_listScissorPending;
    const bool previousActive = g_listScissorActive;
    const bool previousMissionScope = g_missionScissorScope;
    const bool previousNestedPending = g_missionNestedScissorPending;
    const int previousMissionLeft = g_missionNativeScissorLeft;
    const int previousMissionWidth = g_missionNativeScissorWidth;
    const float previousMissionExpansion = g_missionScissorExpansion;
    const auto previousRect = g_listScissorRect;
    const auto previousSource = g_listScissorSource;
    struct Restore {
        bool pending;
        bool active;
        bool missionScope;
        bool nestedPending;
        int missionLeft;
        int missionWidth;
        float missionExpansion;
        display::ScissorRect rect;
        display::ScissorRect source;
        ~Restore() {
            g_listScissorPending = pending;
            g_listScissorActive = active;
            g_missionScissorScope = missionScope;
            g_missionNestedScissorPending = nestedPending;
            g_missionNativeScissorLeft = missionLeft;
            g_missionNativeScissorWidth = missionWidth;
            g_missionScissorExpansion = missionExpansion;
            g_listScissorRect = rect;
            g_listScissorSource = source;
        }
    } restore{previousPending, previousActive, previousMissionScope,
              previousNestedPending, previousMissionLeft,
              previousMissionWidth, previousMissionExpansion,
              previousRect, previousSource};
    g_listScissorRect = rect;
    g_listScissorPending = true;
    g_listScissorActive = false;
    g_missionScissorScope = true;
    g_missionNestedScissorPending = true;
    g_missionNativeScissorLeft = authored.left;
    g_missionNativeScissorWidth = authored.right - authored.left;
    g_missionScissorExpansion = progress;
    return g_missionListDraw(product, pass);
}

void __cdecl ListScissorSubmitHook(int left, int top,
                                  int width, int height) {
    if (!g_listScissorPending) {
        if (g_missionScissorScope && g_missionNestedScissorPending) {
            g_missionNestedScissorPending = false;
            const auto aspect = display::CurrentAspectRatio();
            const float gutter = ff13_ui::HorizontalColumnOffset(
                aspect, ff13_ui::ColumnSide::Right);
            left = g_missionNativeScissorLeft;
            width += static_cast<int>(std::lround(
                2.0f * gutter * g_missionScissorExpansion));
            const display::ScissorRect authored{
                left, top, left + width, top + height};
            g_listScissorRect =
                display::FitAuthoredScissorRectToViewport(authored, aspect);
            g_listScissorSource = {
                std::max(left, 0), std::max(top, 0),
                left + width, top + height};
            g_listScissorActive = true;
        }
        g_nativeScissorSubmit(left, top, width, height);
        return;
    }
    g_listScissorPending = false;
    if (g_missionScissorScope) {
        left = g_missionNativeScissorLeft;
        width = g_missionNativeScissorWidth;
    }
    const auto aspect = display::CurrentAspectRatio();
    const auto trailing = [](int origin, int extent, std::uint32_t limit) {
        return static_cast<std::int32_t>(std::min(
            static_cast<std::int64_t>(origin) + extent,
            static_cast<std::int64_t>(limit)));
    };
    g_listScissorSource = g_missionScissorScope
        ? display::ScissorRect{
            std::max(left, 0), std::max(top, 0),
            left + width, top + height}
        : display::ScissorRect{
            std::max(left, 0), std::max(top, 0),
            trailing(left, width, aspect.width),
            trailing(top, height, aspect.height)};
    if (g_missionScissorScope) g_listScissorActive = true;
    g_nativeScissorSubmit(left, top, width, height);
    g_listScissorActive = true;
}

} // namespace

bool RegisterCalls(patch::Transaction& calls, const ImageView& image) {
    for (const auto rva : kListScissorSubmitCallRvas) {
        const auto site = reinterpret_cast<std::uintptr_t>(Target(image, rva));
        const auto native = reinterpret_cast<std::uintptr_t>(
            Target(image, kNativeScissorSubmitRva));
        const auto expected = patch::EncodeRelativeCall32(site, native);
        const auto replacement = patch::EncodeRelativeCall32(
            site, reinterpret_cast<std::uintptr_t>(
                &ListScissorSubmitHook));
        if (!expected || !replacement ||
            !calls.Add(rva, *expected, *replacement)) return false;
    }
    return true;
}

void Prepare(const ImageView& image) {
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    g_nativeScissorSubmit = reinterpret_cast<NativeScissorSubmitFn>(
        Target(image, kNativeScissorSubmitRva));
}

bool InstallDetours(const ImageView& image) {
    return g_datalogListDrawDetour.Install(
            "ff13-datalog-list-scissor-scope",
            Target(image, kDatalogListDrawRva),
            reinterpret_cast<void*>(&DatalogListDrawHook),
            kDatalogListDrawPrefix,
            reinterpret_cast<void**>(&g_datalogListDraw)) &&
        g_enemySubtypeListDrawDetour.Install(
            "ff13-enemy-subtype-list-scissor-scope",
            Target(image, kEnemySubtypeListDrawRva),
            reinterpret_cast<void*>(&EnemySubtypeListDrawHook),
            kDatalogListDrawPrefix,
            reinterpret_cast<void**>(&g_enemySubtypeListDraw)) &&
        g_settingsListDrawDetour.Install(
            "ff13-settings-list-scissor-scope",
            Target(image, kSettingsListDrawRva),
            reinterpret_cast<void*>(&SettingsListDrawHook),
            kSettingsListDrawPrefix,
            reinterpret_cast<void**>(&g_settingsListDraw)) &&
        g_missionListDrawDetour.Install(
            "ff13-mission-list-scissor-scope",
            Target(image, kMissionListDrawRva),
            reinterpret_cast<void*>(&MissionListDrawHook),
            kMissionListDrawPrefix,
            reinterpret_cast<void**>(&g_missionListDraw));
}

void Revert() {
    g_missionListDrawDetour.RevertAndClear(g_missionListDraw);
    g_settingsListDrawDetour.RevertAndClear(g_settingsListDraw);
    g_enemySubtypeListDrawDetour.RevertAndClear(g_enemySubtypeListDraw);
    g_datalogListDrawDetour.RevertAndClear(g_datalogListDraw);
}

bool ActiveRect(const display::ScissorRect& source,
                display::ScissorRect& output) {
    if (!g_listScissorActive ||
        source.left != g_listScissorSource.left ||
        source.top != g_listScissorSource.top ||
        source.right != g_listScissorSource.right ||
        source.bottom != g_listScissorSource.bottom) return false;
    output = g_listScissorRect;
    return true;
}

} // namespace novafix::game::ff13_ui::menu_list_clip
