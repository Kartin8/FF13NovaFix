#include "game/titles/ff13/ui/menu/menu_chrome.h"

#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "game/titles/ff13/ui/menu/menu_column_layout.h"
#include "game/titles/ff13/ui/menu/menu_contract.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace novafix::game::ff13_ui::menu_chrome {
namespace {

using menu_contract::kPartyStatusValueVtableRva;
using menu_contract::kBackdropVtableRva;
using menu_contract::kNodeParentOffset;
using menu_contract::kNodeFirstChildOffset;
using menu_contract::kNodeNextSiblingOffset;
using menu_contract::kNodeDirtyFlagsOffset;

constexpr std::uint32_t kMainMenuUpdateRva = 0x9B5720u - 0x400000u;
constexpr std::size_t kChromeContainerOffset = 0xA0u;
constexpr std::array<std::byte, 6> kMainMenuUpdatePrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x7C}};

using MainMenuUpdateFn = int (__thiscall*)(void*, float);

patch::Detour32 g_mainMenuUpdateDetour;
MainMenuUpdateFn g_mainMenuUpdate{};
void* g_backdropVtable{};
std::uintptr_t g_imageBase{};
thread_local Context g_chromeUpdate{};

template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

void* Target(const ImageView& image, std::uint32_t rva) {
    const auto range = image.At(rva, 1u);
    return range ? const_cast<std::byte*>(range->data()) : nullptr;
}

class ScopedChromeUpdate {
public:
    explicit ScopedChromeUpdate(void* menu) : previous_(g_chromeUpdate) {
        g_chromeUpdate = {};
        const auto aspect = display::CurrentAspectRatio();
        if (!menu || !aspect.valid || !aspect.correctionRequired) return;
        const int mode = Field<int>(menu, 0x88u);
        if (mode == 3 || mode == 4) return;
        void* container = Field<void*>(menu, kChromeContainerOffset);
        if (!container || !patch::memory::IsReadable(container, 0x18u)) return;
        void* header = Field<void*>(container, kNodeFirstChildOffset);
        if (!header || !patch::memory::IsWritable(
                header, kNodeDirtyFlagsOffset + sizeof(std::uint32_t))) return;
        void* footer = Field<void*>(header, kNodeNextSiblingOffset);
        if (!footer || !patch::memory::IsWritable(
                footer, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) ||
            Field<void*>(header, kNodeParentOffset) != container ||
            Field<void*>(footer, kNodeParentOffset) != container ||
            Field<void*>(footer, kNodeNextSiblingOffset) != nullptr) return;
        const float offset = 360.0f * (1.0f / aspect.safeScaleY - 1.0f);
        if (!std::isfinite(offset)) return;
        g_chromeUpdate = {header, footer, offset};
        void* backdrop = Field<void*>(menu, 0x9Cu);
        if (backdrop && patch::memory::IsWritable(
                backdrop, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) &&
            Field<void*>(backdrop, 0u) == g_backdropVtable) {
            g_chromeUpdate.backdrop = backdrop;
        }
        g_chromeUpdate.widthScale = 1.0f / aspect.safeScaleX;
        g_chromeUpdate.heightScale = 1.0f / aspect.safeScaleY;
        void* gil = Field<void*>(menu, 0x90u);
        void* time = Field<void*>(menu, 0x98u);
        void* guide = Field<void*>(menu, 0xB0u);
        const auto footerValue = [footer](void* node) {
            return node && patch::memory::IsWritable(
                    node, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) &&
                Field<void*>(node, kNodeParentOffset) == footer &&
                reinterpret_cast<std::uintptr_t>(Field<void*>(node, 0u)) ==
                    g_imageBase + kPartyStatusValueVtableRva;
        };
        if (aspect.safeScaleX < 1.0f &&
            footerValue(gil) && footerValue(time)) {
            g_chromeUpdate.footerGil = gil;
            g_chromeUpdate.footerTime = time;
            g_chromeUpdate.footerValueOffset =
                ff13_ui::HorizontalColumnOffset(
                    aspect, ff13_ui::ColumnSide::Left);
        }
        if (aspect.safeScaleX < 1.0f && footerValue(guide)) {
            g_chromeUpdate.footerGuide = guide;
            g_chromeUpdate.footerGuideOffset =
                ff13_ui::HorizontalColumnOffset(
                    aspect, ff13_ui::ColumnSide::Right);
        }
        if (aspect.safeScaleX < 1.0f) {
            const auto backing = [](void* root) -> void* {
                void* child = Field<void*>(root, kNodeFirstChildOffset);
                return child && patch::memory::IsWritable(
                        child, kNodeDirtyFlagsOffset + sizeof(std::uint32_t)) &&
                    Field<void*>(child, kNodeParentOffset) == root &&
                    Field<void*>(child, kNodeFirstChildOffset) == nullptr
                    ? child : nullptr;
            };
            g_chromeUpdate.headerBacking = backing(header);
            g_chromeUpdate.footerBacking = backing(footer);
            g_chromeUpdate.widthScale = 1.0f / aspect.safeScaleX;
        }
    }

    ~ScopedChromeUpdate() { g_chromeUpdate = previous_; }

    ScopedChromeUpdate(const ScopedChromeUpdate&) = delete;
    ScopedChromeUpdate& operator=(const ScopedChromeUpdate&) = delete;

private:
    Context previous_{};
};

int __fastcall MainMenuUpdateHook(void* menu, void*, float delta) {
    const ScopedChromeUpdate context(menu);
    return g_mainMenuUpdate(menu, delta);
}

} // namespace

const Context& Current() {
    return g_chromeUpdate;
}

void Prepare(const ImageView& image) {
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    g_backdropVtable = Target(image, kBackdropVtableRva);
}

bool InstallDetour(const ImageView& image) {
    return g_mainMenuUpdateDetour.Install(
        "ff13-main-menu-chrome-anchor", Target(image, kMainMenuUpdateRva),
        reinterpret_cast<void*>(&MainMenuUpdateHook), kMainMenuUpdatePrefix,
        reinterpret_cast<void**>(&g_mainMenuUpdate));
}

void Revert() {
    g_mainMenuUpdateDetour.RevertAndClear(g_mainMenuUpdate);
}

} // namespace novafix::game::ff13_ui::menu_chrome
