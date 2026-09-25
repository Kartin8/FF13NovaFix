#include "game/titles/ff13/ui/menu/menu_aspect.h"

#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "game/titles/ff13/ui/menu/menu_column_layout.h"
#include "game/titles/ff13/ui/menu/menu_chrome.h"
#include "game/titles/ff13/ui/menu/menu_contract.h"
#include "game/titles/ff13/ui/menu/menu_list_clip.h"
#include "game/titles/ff13/ui/menu/menu_layout.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::ff13_ui::menu_aspect {
namespace {

using menu_contract::kSettingsListVtableRva;
using menu_contract::kNodeParentOffset;

constexpr std::uint32_t kMainMenuProductVtableRva =
    0x11549A8u - 0x400000u;
constexpr std::uint32_t kSettingsProductVtableRva =
    0x114D4D0u - 0x400000u;
constexpr std::uint32_t kSettingsCursorRva =
    0x973040u - 0x400000u;
constexpr std::uint32_t kPartyStatusStripVtableRva =
    0x119F010u - 0x400000u;
constexpr std::uint32_t kPartyStatusStripDrawRva =
    0xA637D0u - 0x400000u;
constexpr std::size_t kSettingsListOffset = 0x7Cu;
constexpr std::uint32_t kListCursorRva = 0xA53AC0u - 0x400000u;
constexpr std::uint32_t kEquipmentCursorRva = 0xA49DE0u - 0x400000u;
constexpr std::uint32_t kAccessoryCursorRva = 0xA4EA40u - 0x400000u;
constexpr std::uint32_t kEnemyCategoryCursorRva = 0xA509E0u - 0x400000u;
constexpr std::uint32_t kArticleCursorRva = 0x95ACA0u - 0x400000u;
constexpr std::size_t kPartyStatusRootOffset = 0x90u;
constexpr std::size_t kResourceIdOffset = 0xC0u;
constexpr std::size_t kNodeOwnerOffset = 0xC8u;
constexpr std::array<std::byte, 12> kPartyStatusStripDrawPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xE4}, std::byte{0xF0}, std::byte{0x81}, std::byte{0xEC},
    std::byte{0xF0}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00}};
constexpr std::array<std::byte, 9> kSettingsCursorPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x81},
    std::byte{0xEC}, std::byte{0xB4}, std::byte{0x02}, std::byte{0x00},
    std::byte{0x00}};
constexpr std::array<std::byte, 9> kListCursorPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x2C}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xD8}};
constexpr std::array<std::byte, 9> kEquipmentCursorPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x81},
    std::byte{0xEC}, std::byte{0x30}, std::byte{0x01}, std::byte{0x00},
    std::byte{0x00}};
constexpr std::array<std::byte, 9> kAccessoryCursorPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x81},
    std::byte{0xEC}, std::byte{0x88}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}};
constexpr std::array<std::byte, 9> kEnemyCategoryCursorPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x34}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xD0}};
constexpr std::array<std::byte, 9> kArticleCursorPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x81},
    std::byte{0xEC}, std::byte{0x38}, std::byte{0x02}, std::byte{0x00},
    std::byte{0x00}};

using CursorPositionFn = void* (__thiscall*)(void*, float*);
using PartyStatusStripDrawFn = void (__thiscall*)(void*, const float*);

patch::Detour32 g_listCursorDetour;
patch::Detour32 g_equipmentCursorDetour;
patch::Detour32 g_accessoryCursorDetour;
patch::Detour32 g_enemyCategoryCursorDetour;
patch::Detour32 g_articleCursorDetour;
patch::Detour32 g_partyStatusStripDrawDetour;
patch::Detour32 g_settingsCursorDetour;
CursorPositionFn g_listCursor{};
CursorPositionFn g_equipmentCursor{};
CursorPositionFn g_accessoryCursor{};
CursorPositionFn g_enemyCategoryCursor{};
CursorPositionFn g_articleCursor{};
PartyStatusStripDrawFn g_partyStatusStripDraw{};
CursorPositionFn g_settingsCursor{};
std::uintptr_t g_imageBase{};
template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}
void* __fastcall ListCursorHook(void* widget, void*, float* position) {
    void* result = g_listCursor(widget, position);
    menu_layout::CorrectLocalCursor(widget, position);
    return result;
}

void* __fastcall SettingsCursorHook(void* widget, void*, float* position) {
    void* result = g_settingsCursor(widget, position);
    if (!widget || !position || position[3] == 0.0f ||
        g_imageBase == 0u ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(widget, 0u)) !=
            g_imageBase + kSettingsListVtableRva) return result;
    void* owner = Field<void*>(widget, kNodeOwnerOffset);
    if (!owner || !patch::memory::IsReadable(
            owner, kSettingsListOffset + sizeof(void*)) ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(owner, 0u)) !=
            g_imageBase + kSettingsProductVtableRva ||
        Field<void*>(owner, kSettingsListOffset) != widget) return result;
    position[0] -= ff13_ui::HorizontalColumnOffset(
        display::CurrentAspectRatio(), ff13_ui::ColumnSide::Right);
    return result;
}

void* __fastcall EquipmentCursorHook(void* widget, void*, float* position) {
    void* result = g_equipmentCursor(widget, position);
    menu_layout::CorrectLocalCursor(widget, position);
    return result;
}

void* __fastcall AccessoryCursorHook(void* widget, void*, float* position) {
    void* result = g_accessoryCursor(widget, position);
    menu_layout::CorrectLocalCursor(widget, position);
    return result;
}

void* __fastcall EnemyCategoryCursorHook(void* widget, void*, float* position) {
    void* result = g_enemyCategoryCursor(widget, position);
    menu_layout::CorrectLocalCursor(widget, position);
    return result;
}

void* __fastcall ArticleCursorHook(void* product, void*, float* position) {
    void* result = g_articleCursor(product, position);
    if (position && position[3] != 0.0f &&
        Field<std::uint8_t>(product, 0x3Au) != 5u &&
        menu_layout::HasColumnProfile(product)) {
        position[0] += ff13_ui::HorizontalColumnOffset(
            display::CurrentAspectRatio(), ff13_ui::ColumnSide::Right);
    }
    return result;
}

void __fastcall PartyStatusStripDrawHook(
    void* node, void*, const float* matrix) {
    const auto aspect = display::CurrentAspectRatio();
    if (!aspect.valid || aspect.safeScaleX >= 1.0f) {
        g_partyStatusStripDraw(node, matrix);
        return;
    }
    if (!node || matrix || Field<int>(node, kResourceIdOffset) != 907 ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(node, 0u)) !=
            g_imageBase + kPartyStatusStripVtableRva) {
        g_partyStatusStripDraw(node, matrix);
        return;
    }
    void* owner = Field<void*>(node, kNodeOwnerOffset);
    if (!owner || !patch::memory::IsReadable(
            owner, kPartyStatusRootOffset + sizeof(void*)) ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(owner, 0u)) !=
            g_imageBase + kMainMenuProductVtableRva ||
        Field<void*>(node, kNodeParentOffset) !=
            Field<void*>(owner, kPartyStatusRootOffset)) {
        g_partyStatusStripDraw(node, matrix);
        return;
    }
    // A637D0 treats a non-null override as two adjacent 4x4 blocks:
    // node+0x18 and node+0x58. Preserve the second block verbatim.
    std::array<float, 32> widened{};
    std::memcpy(widened.data(),
                static_cast<std::byte*>(node) + 0x18u,
                sizeof(widened));
    const float widthScale = 1.0f / aspect.safeScaleX;
    for (const std::size_t index : {0u, 4u, 8u}) {
        widened[index] *= widthScale;
    }
    widened[12] = 640.0f + (widened[12] - 640.0f) * widthScale;
    g_partyStatusStripDraw(node, widened.data());
}

void* Target(const ImageView& image, std::uint32_t rva) {
    const auto range = image.At(rva, 1u);
    return range ? const_cast<std::byte*>(range->data()) : nullptr;
}

} // namespace

bool RegisterProductUpdatePatch(patch::Transaction& calls,
                                const ImageView& image) {
    return menu_layout::RegisterProductUpdatePatch(calls, image);
}

bool RegisterNodeTransformCall(patch::Transaction& calls,
                               const ImageView& image) {
    return menu_layout::RegisterNodeTransformCall(calls, image);
}

bool RegisterListScissorCalls(patch::Transaction& calls,
                              const ImageView& image) {
    return menu_list_clip::RegisterCalls(calls, image);
}

void Prepare(const ImageView& image) {
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    menu_chrome::Prepare(image);
    menu_layout::Prepare(image);
    menu_list_clip::Prepare(image);
}

bool InstallDetours(const ImageView& image) {
    return menu_chrome::InstallDetour(image) &&
        g_listCursorDetour.Install(
            "ff13-list-cursor-anchor", Target(image, kListCursorRva),
            reinterpret_cast<void*>(&ListCursorHook), kListCursorPrefix,
            reinterpret_cast<void**>(&g_listCursor)) &&
        g_settingsCursorDetour.Install(
            "ff13-settings-cursor-anchor", Target(image, kSettingsCursorRva),
            reinterpret_cast<void*>(&SettingsCursorHook),
            kSettingsCursorPrefix,
            reinterpret_cast<void**>(&g_settingsCursor)) &&
        g_equipmentCursorDetour.Install(
            "ff13-equipment-cursor-anchor", Target(image, kEquipmentCursorRva),
            reinterpret_cast<void*>(&EquipmentCursorHook), kEquipmentCursorPrefix,
            reinterpret_cast<void**>(&g_equipmentCursor)) &&
        g_accessoryCursorDetour.Install(
            "ff13-accessory-cursor-anchor", Target(image, kAccessoryCursorRva),
            reinterpret_cast<void*>(&AccessoryCursorHook), kAccessoryCursorPrefix,
            reinterpret_cast<void**>(&g_accessoryCursor)) &&
        g_enemyCategoryCursorDetour.Install(
            "ff13-enemy-category-cursor-anchor",
            Target(image, kEnemyCategoryCursorRva),
            reinterpret_cast<void*>(&EnemyCategoryCursorHook),
            kEnemyCategoryCursorPrefix,
            reinterpret_cast<void**>(&g_enemyCategoryCursor)) &&
        g_articleCursorDetour.Install(
            "ff13-article-entry-cursor-anchor", Target(image, kArticleCursorRva),
            reinterpret_cast<void*>(&ArticleCursorHook), kArticleCursorPrefix,
            reinterpret_cast<void**>(&g_articleCursor)) &&
        g_partyStatusStripDrawDetour.Install(
            "ff13-party-status-strip-aspect",
            Target(image, kPartyStatusStripDrawRva),
            reinterpret_cast<void*>(&PartyStatusStripDrawHook),
            kPartyStatusStripDrawPrefix,
            reinterpret_cast<void**>(&g_partyStatusStripDraw)) &&
        menu_list_clip::InstallDetours(image);
}

void Revert() {
    menu_list_clip::Revert();
    g_settingsCursorDetour.RevertAndClear(g_settingsCursor);
    g_partyStatusStripDrawDetour.RevertAndClear(g_partyStatusStripDraw);
    g_articleCursorDetour.RevertAndClear(g_articleCursor);
    g_enemyCategoryCursorDetour.RevertAndClear(g_enemyCategoryCursor);
    g_accessoryCursorDetour.RevertAndClear(g_accessoryCursor);
    g_equipmentCursorDetour.RevertAndClear(g_equipmentCursor);
    g_listCursorDetour.RevertAndClear(g_listCursor);
    menu_chrome::Revert();
}

bool ActiveGuiListScissorRect(const display::ScissorRect& source,
                             display::ScissorRect& output) {
    return menu_list_clip::ActiveRect(source, output);
}

} // namespace novafix::game::ff13_ui::menu_aspect
