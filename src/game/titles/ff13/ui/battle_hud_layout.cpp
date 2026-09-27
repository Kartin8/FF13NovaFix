#include "game/titles/ff13/ui/battle_hud_layout.h"

#include "game/core/image_view.h"
#include "display/aspect_runtime.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <algorithm>
#include <array>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace novafix::game::ff13_ui::battle_hud_layout {
namespace {

constexpr std::uint32_t kGuiManagerPointerRva = 0x2826A4Cu - 0x400000u;
constexpr std::uint32_t kCursorProductVtableRva = 0x11499E8u - 0x400000u;
constexpr std::uint32_t kCursorTargetRva = 0x944150u - 0x400000u;
constexpr std::array<std::byte, 9> kCursorTargetPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x1C},
    std::byte{0x89}, std::byte{0x4D}, std::byte{0xE4}};

using CursorTargetFn = int (__thiscall*)(void*, float, float, float, float);
patch::Detour32 g_cursorTargetDetour;
CursorTargetFn g_cursorTarget{};
void** g_guiManagerPointer{};
std::uintptr_t g_imageBase{};

struct Context {
    void* root{};
    float offset{};
};
thread_local Context g_context{};

template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

bool LowerHudProduct(std::uintptr_t vtableRva) {
    // Product roots, not individual authored strips: status icons and
    // mod-added descendants inherit one translation.
    switch (vtableRva) {
    case 0x1146338u - 0x400000u: // BtlTopCmd
    case 0x11416B8u - 0x400000u: // BtlAblList
    case 0x1146A58u - 0x400000u: // Techniques
    case 0x1143B38u - 0x400000u: // BtlItem
    case 0x1145DA0u - 0x400000u: // BtlTgtList
    case 0x1142FD8u - 0x400000u: // BtlForma (paradigm list)
    case 0x11421E0u - 0x400000u: // BtlCmdStk
    case 0x1143400u - 0x400000u: // BtlHpAtb
    case 0x1144418u - 0x400000u: // BtlOptInfo (portraits/descriptions)
    case 0x11446D8u - 0x400000u: // BtlOptName
    case 0x1142628u - 0x400000u: // BtlDriveBtn
    case 0x1146CD0u - 0x400000u: // BtlTpGauge
        return true;
    default: // Chain/stagger, target frames, world labels and result UI.
        return false;
    }
}

bool UpperHudProduct(std::uintptr_t vtableRva) {
    return vtableRva == 0x1141B98u - 0x400000u; // BtlChain/stagger
}

float VerticalGutter(const display::AspectRatio& aspect) {
    if (!aspect.valid || !std::isfinite(aspect.safeScaleY) ||
        aspect.safeScaleY <= 0.0f || aspect.safeScaleY >= 1.0f) return 0.0f;
    const float gutter = 360.0f * (1.0f / aspect.safeScaleY - 1.0f);
    return std::isfinite(gutter) ? gutter : 0.0f;
}

void* Target(const ImageView& image, std::uint32_t rva) {
    const auto range = image.At(rva, 1u);
    return range ? const_cast<std::byte*>(range->data()) : nullptr;
}

int __fastcall CursorTargetHook(void* cursor, void*, float x, float y,
                                float z, float weight) {
    const float gutter = VerticalGutter(display::CurrentAspectRatio());
    if (gutter == 0.0f)
        return g_cursorTarget(cursor, x, y, z, weight);
    if (cursor && weight != 0.0f && g_guiManagerPointer &&
        g_imageBase != 0u &&
        reinterpret_cast<std::uintptr_t>(Field<void*>(cursor, 0u)) ==
            g_imageBase + kCursorProductVtableRva) {
        void* const manager = *g_guiManagerPointer;
        if (manager && patch::memory::IsReadable(manager, 2256u)) {
            void* const focused = Field<void*>(manager, 724u);
            if (focused && patch::memory::IsReadable(focused, 0x50u)) {
                const auto vtable = reinterpret_cast<std::uintptr_t>(
                    Field<void*>(focused, 0u));
                if (vtable >= g_imageBase &&
                    LowerHudProduct(vtable - g_imageBase)) {
                    // The cursor getter returns authored/local coordinates,
                    // while the menu root has already been translated. Battle
                    // command products have no resource at +0x4C; use their
                    // actual root transform rather than a resource-owner gate.
                    void* const root = Field<void*>(focused, 0x34u);
                    if (root &&
                        patch::memory::IsReadable(root, 0xD0u) &&
                        reinterpret_cast<std::uintptr_t>(Field<void*>(root, 0u)) ==
                            g_imageBase + (0x119C470u - 0x400000u)) {
                        const float translation = Field<float>(root, 0x4Cu) -
                                                  Field<float>(root, 0xACu);
                        if (std::isfinite(translation)) y += translation;
                    }
                }
            }
        }
    }
    return g_cursorTarget(cursor, x, y, z, weight);
}

} // namespace

ScopedProduct::ScopedProduct(
    void* product, const display::AspectRatio& aspect,
    std::uintptr_t imageBase, void* battleResource)
    : previousRoot_(g_context.root), previousOffset_(g_context.offset) {
    g_context = {};
    if (!product || imageBase == 0u) return;
    const float gutter = VerticalGutter(aspect);
    if (gutter == 0.0f) return;
    const auto vtable = reinterpret_cast<std::uintptr_t>(Field<void*>(product, 0u));
    if (vtable < imageBase) return;
    const auto vtableRva = vtable - imageBase;
    const bool lower = LowerHudProduct(vtableRva);
    const bool upper = UpperHudProduct(vtableRva);
    if (!lower && !upper) return;
    // The TP product is also instantiated by non-battle menus.
    if ((vtableRva == 0x1146CD0u - 0x400000u || upper) &&
        (!battleResource || Field<void*>(product, 0x4Cu) != battleResource)) return;
    void* const root = Field<void*>(product, 0x34u);
    if (!root || !patch::memory::IsWritable(root, 0xD0u) ||
        reinterpret_cast<std::uintptr_t>(Field<void*>(root, 0u)) !=
            imageBase + (0x119C470u - 0x400000u)) return;
    // Give the paradigm/Gestalt banners extra clearance above the ATB row.
    // Translate the whole root so text, backing and effects stay aligned.
    const bool banner = vtableRva == 0x11446D8u - 0x400000u ||
                        vtableRva == 0x1142628u - 0x400000u;
    const float bannerLift = banner ? std::min(gutter, 24.0f) : 0.0f;
    g_context = {root, upper ? -gutter : gutter - bannerLift};
}

ScopedProduct::~ScopedProduct() {
    g_context = {previousRoot_, previousOffset_};
}

bool Affects(void* node) {
    return node && node == g_context.root;
}

void Apply(void* node) {
    Field<float>(node, 0x4Cu) += g_context.offset;
}

void Prepare(const ImageView& image) {
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    g_guiManagerPointer = reinterpret_cast<void**>(
        Target(image, kGuiManagerPointerRva));
}

bool InstallDetour(const ImageView& image) {
    return g_cursorTargetDetour.Install(
        "ff13-battle-cursor-vertical-anchor",
        Target(image, kCursorTargetRva),
        reinterpret_cast<void*>(&CursorTargetHook), kCursorTargetPrefix,
        reinterpret_cast<void**>(&g_cursorTarget));
}

void Revert() {
    g_cursorTargetDetour.RevertAndClear(g_cursorTarget);
}

} // namespace novafix::game::ff13_ui::battle_hud_layout
