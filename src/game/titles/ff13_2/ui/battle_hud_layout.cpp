#include "game/titles/ff13_2/ui/battle_hud_layout.h"

#include "diagnostics/log.h"
#include "display/aspect_runtime.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/ff13_2/ui/menu_chrome.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_battle_hud_layout {
namespace {

constexpr std::uint32_t kProductUpdateRva = 0x892770u - 0x400000u;
constexpr std::uint32_t kNodeTransformRva = 0x777AD0u - 0x400000u;
constexpr std::uint32_t kCursorTargetRva = 0x7DD9D0u - 0x400000u;
constexpr std::uint32_t kGuiManagerPointerRva = 0x514DE0Cu - 0x400000u;
constexpr std::array<std::byte, 13> kProductUpdatePrefix{
    std::byte{0x56}, std::byte{0x8B}, std::byte{0xF1},
    std::byte{0x83}, std::byte{0x7E}, std::byte{0x34},
    std::byte{0x00}, std::byte{0x0F}, std::byte{0x84},
    std::byte{0x81}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00},
};
constexpr std::array<std::byte, 13> kNodeTransformPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x83}, std::byte{0xE4}, std::byte{0xF0},
    std::byte{0x81}, std::byte{0xEC}, std::byte{0x24},
    std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x53},
};
constexpr std::array<std::byte, 13> kCursorTargetPrefix{
    std::byte{0xF3}, std::byte{0x0F}, std::byte{0x10},
    std::byte{0x4C}, std::byte{0x24}, std::byte{0x10},
    std::byte{0x0F}, std::byte{0x57}, std::byte{0xC0},
    std::byte{0x0F}, std::byte{0x2E}, std::byte{0xC8},
    std::byte{0x9F},
};

constexpr std::size_t kProductRootOffset = 0x34u;
constexpr std::size_t kProductNextOffset = 0x08u;
constexpr std::size_t kManagerProductsOffset = 996u;
constexpr std::size_t kManagerFocusedProductOffset = 1008u;
constexpr std::size_t kNodeWorldYOffset = 0x4Cu;
constexpr std::size_t kNodeFlagsOffset = 0xCCu;
constexpr std::uint32_t kTransformDirty = 0x04u;
constexpr float kAuthoredHalfHeight = 360.0f;

using ProductUpdateFn = void (__thiscall*)(void*, float);
using NodeTransformFn = void (__thiscall*)(void*, float);
using CursorTargetFn = int (__thiscall*)(void*, float, float, float, float);

patch::Detour32 g_productUpdateDetour;
patch::Detour32 g_nodeTransformDetour;
patch::Detour32 g_cursorTargetDetour;
ProductUpdateFn g_productUpdate{};
NodeTransformFn g_nodeTransform{};
CursorTargetFn g_cursorTarget{};
const std::byte* g_imageBase{};
void** g_guiManagerPointer{};

struct UpdateContext {
    void* root{};
    float offset{};
    xiii2_menu_chrome::VerticalAnchors menu{};
    float menuGutter{};
};
thread_local UpdateContext g_updateContext{};

template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

std::uintptr_t VtableRva(void* object) {
    const auto vtable = reinterpret_cast<std::uintptr_t>(Field<void*>(object, 0));
    const auto base = reinterpret_cast<std::uintptr_t>(g_imageBase);
    return vtable >= base ? vtable - base : 0u;
}

bool LowerListProduct(std::uintptr_t rva) {
    // Seven battle command/list variants share the native selected-row
    // getter at 0x7BDC30. It returns local coordinates, not world position.
    switch (rva) {
    case 0xDE1FA0u - 0x400000u:
    case 0xDE3F68u - 0x400000u:
    case 0xDE4B78u - 0x400000u:
    case 0xDE4E20u - 0x400000u:
    case 0xDE6DD0u - 0x400000u:
    case 0xDE7208u - 0x400000u:
    case 0xDE7640u - 0x400000u:
        return true;
    default:
        return false;
    }
}

bool LowerProduct(std::uintptr_t rva) {
    if (LowerListProduct(rva)) return true;
    switch (rva) {
    case 0xDE42B0u - 0x400000u: // Party status and HP
    case 0xDE32B0u - 0x400000u: // Command wheel and ATB graphics
    case 0xDE3548u - 0x400000u: // Command selection overlay
    case 0xDE5AD8u - 0x400000u: // Paradigm Shift banner
    case 0xDE39C8u - 0x400000u: // Right-hand battle banner
    case 0xDE3C98u - 0x400000u: // Status icons / Feral Link
        return true;
    default: // World labels, target reticles and stagger are not HUD roots.
        return false;
    }
}

bool BattleActive() {
    if (!g_guiManagerPointer ||
        !patch::memory::IsReadable(g_guiManagerPointer, sizeof(void*))) return false;
    void* const manager = *g_guiManagerPointer;
    if (!manager || !patch::memory::IsReadable(
            manager, kManagerProductsOffset + sizeof(void*))) return false;
    void* product = Field<void*>(manager, kManagerProductsOffset);
    bool wheel = false;
    bool party = false;
    for (unsigned i = 0; i < 128 && product; ++i) {
        if (!patch::memory::IsReadable(product, 12u)) return false;
        const auto vtable = VtableRva(product);
        wheel |= vtable == 0xDE32B0u - 0x400000u;
        party |= vtable == 0xDE42B0u - 0x400000u;
        if (wheel && party) return true;
        product = Field<void*>(product, kProductNextOffset);
    }
    return false;
}

float VerticalGutter() {
    const auto aspect = display::CurrentAspectRatio();
    if (!aspect.valid || !std::isfinite(aspect.safeScaleY) ||
        aspect.safeScaleY <= 0.0f || aspect.safeScaleY >= 1.0f) return 0.0f;
    const float gutter = kAuthoredHalfHeight *
        (1.0f / aspect.safeScaleY - 1.0f);
    return std::isfinite(gutter) ? gutter : 0.0f;
}

void __fastcall ProductUpdateHook(void* product, void*, float delta) {
    const float gutter = VerticalGutter();
    if (gutter == 0.0f) {
        g_productUpdate(product, delta);
        return;
    }
    const UpdateContext previous = g_updateContext;
    g_updateContext = {};
    g_updateContext.menu = xiii2_menu_chrome::CaptureVerticalAnchors(
        product, reinterpret_cast<std::uintptr_t>(g_imageBase));
    g_updateContext.menuGutter = gutter;
    if (product) {
        const auto vtable = VtableRva(product);
        const bool lower = LowerProduct(vtable);
        const bool help = vtable == 0xDEBE70u - 0x400000u;
        // These product classes also occur in non-battle menus. Never move
        // either HUD or cursor-bearing descendants without the battle list.
        if ((lower || help) && BattleActive()) {
            void* const root = Field<void*>(product, kProductRootOffset);
            if (root && patch::memory::IsWritable(root, kNodeFlagsOffset + 4u)) {
                g_updateContext.root = root;
                g_updateContext.offset = help ? -gutter : gutter;
                Field<std::uint32_t>(root, kNodeFlagsOffset) |= kTransformDirty;
            }
        }
    }
    g_productUpdate(product, delta);
    g_updateContext = previous;
}

void __fastcall NodeTransformHook(void* node, void*, float delta) {
    const auto& menu = g_updateContext.menu;
    const bool menuHeader = xiii2_menu_chrome::IsHeader(
        node, menu, reinterpret_cast<std::uintptr_t>(g_imageBase));
    const bool menuNode = node &&
        (menuHeader || node == menu.footers[0] || node == menu.footers[1]);
    const float offset = g_updateContext.root && node == g_updateContext.root
        ? g_updateContext.offset
        : menuNode ? (menuHeader ? -g_updateContext.menuGutter
                                : g_updateContext.menuGutter)
        : 0.0f;
    const bool translate = node && offset != 0.0f &&
        (Field<std::uint32_t>(node, kNodeFlagsOffset) & 1u) == 0u;
    if (translate) Field<std::uint32_t>(node, kNodeFlagsOffset) |= kTransformDirty;
    g_nodeTransform(node, delta);
    if (translate) {
        Field<float>(node, kNodeWorldYOffset) += offset;
        // Children resolve their matrices from this corrected parent.
        Field<std::uint32_t>(node, kNodeFlagsOffset) |= 8u;
        // Rebuild from native animation channels next update, even if the
        // viewport returns to 16:9 and no longer needs menu anchoring.
        if (menuNode) Field<std::uint32_t>(node, kNodeFlagsOffset) |= kTransformDirty;
    }
}

int __fastcall CursorTargetHook(void* cursor, void*, float x, float y,
                                float z, float weight) {
    const float gutter = VerticalGutter();
    if (gutter == 0.0f)
        return g_cursorTarget(cursor, x, y, z, weight);
    if (cursor && weight != 0.0f &&
        VtableRva(cursor) == 0xDEB628u - 0x400000u &&
        g_guiManagerPointer &&
        patch::memory::IsReadable(g_guiManagerPointer, sizeof(void*))) {
        void* const manager = *g_guiManagerPointer;
        if (manager && patch::memory::IsReadable(
                manager, kManagerFocusedProductOffset + sizeof(void*))) {
            void* const focused = Field<void*>(
                manager, kManagerFocusedProductOffset);
            if (focused && patch::memory::IsReadable(focused, 4u) &&
                LowerListProduct(VtableRva(focused)) && BattleActive()) {
                // The selected-row getter reports authored/local Y. Translate
                // its target once; never translate the global cursor root.
                y += gutter;
            }
        }
    }
    return g_cursorTarget(cursor, x, y, z, weight);
}

template <std::size_t N>
bool Matches(const ImageView& image, std::uint32_t rva,
             const std::array<std::byte, N>& prefix) {
    const auto bytes = image.At(rva, prefix.size());
    return bytes && std::memcmp(bytes->data(), prefix.data(), N) == 0;
}

BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::ArbitraryAspectRatio)) return TRUE;
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image || !Matches(*image, kProductUpdateRva, kProductUpdatePrefix) ||
        !Matches(*image, kNodeTransformRva, kNodeTransformPrefix) ||
        !Matches(*image, kCursorTargetRva, kCursorTargetPrefix)) {
        LogWarning("XIII-2 battle HUD layout disabled: executable contract mismatch");
        return TRUE;
    }
    g_imageBase = image->Bytes().data();
    g_guiManagerPointer = reinterpret_cast<void**>(const_cast<std::byte*>(
        image->At(kGuiManagerPointerRva, 1u)->data()));
    const auto target = [&image](std::uint32_t rva) {
        return const_cast<std::byte*>(image->At(rva, 1u)->data());
    };
    const bool productInstalled = g_productUpdateDetour.Install(
        "ff13-2-battle-hud-product", target(kProductUpdateRva),
        reinterpret_cast<void*>(&ProductUpdateHook), kProductUpdatePrefix,
        reinterpret_cast<void**>(&g_productUpdate));
    const bool nodeInstalled = productInstalled && g_nodeTransformDetour.Install(
        "ff13-2-battle-hud-node", target(kNodeTransformRva),
        reinterpret_cast<void*>(&NodeTransformHook), kNodeTransformPrefix,
        reinterpret_cast<void**>(&g_nodeTransform));
    const bool cursorInstalled = nodeInstalled && g_cursorTargetDetour.Install(
        "ff13-2-battle-cursor-target", target(kCursorTargetRva),
        reinterpret_cast<void*>(&CursorTargetHook), kCursorTargetPrefix,
        reinterpret_cast<void**>(&g_cursorTarget));
    if (!cursorInstalled) {
        g_cursorTargetDetour.RevertAndClear(g_cursorTarget);
        g_nodeTransformDetour.RevertAndClear(g_nodeTransform);
        g_productUpdateDetour.RevertAndClear(g_productUpdate);
        LogWarning("XIII-2 battle HUD layout disabled: detour installation failed");
    }
    return TRUE;
}

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &InitializeOnce, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_battle_hud_layout
