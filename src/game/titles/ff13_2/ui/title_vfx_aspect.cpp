#include "game/titles/ff13_2/ui/title_vfx_aspect.h"

#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_ui::title_vfx_aspect {
namespace {

constexpr std::uint32_t kGuiManagerGlobalRva = 0x04D4DE0Cu;
constexpr std::uint32_t kTitleVisualVtableRva = 0x00E42740u - 0x00400000u;
constexpr std::uint32_t kVfxDrawRva = 0x00ACC5E0u - 0x00400000u;
constexpr std::uint32_t kVfxGeometryDrawRva = 0x00AA1CC0u - 0x00400000u;
constexpr std::array<std::byte, 6> kVfxDrawPrefix{
    std::byte{0x56}, std::byte{0x8B}, std::byte{0xF1},
    std::byte{0x8B}, std::byte{0x4E}, std::byte{0x04}};
constexpr std::array<std::byte, 7> kVfxGeometryDrawPrefix{
    std::byte{0x80}, std::byte{0xB9}, std::byte{0x8F}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};

using VfxDrawFn = int (__thiscall*)(void*);
using VfxGeometryDrawFn = int (__thiscall*)(
    void*, void*, void*, const float*);

patch::Detour32 g_drawDetour;
patch::Detour32 g_geometryDetour;
VfxDrawFn g_draw{};
VfxGeometryDrawFn g_geometryDraw{};
std::byte* g_imageBase{};
thread_local bool g_safeTitlePart{};
thread_local DWORD g_lastModeCheck{};
thread_local int g_cachedMode{};

template <typename T>
T Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

int ActiveTitleMode() {
    const DWORD now = GetTickCount();
    if (now - g_lastModeCheck < 16u) return g_cachedMode;
    g_lastModeCheck = now;
    g_cachedMode = 0;
    if (!g_imageBase) return 0;
    auto** const slot = reinterpret_cast<void**>(
        g_imageBase + kGuiManagerGlobalRva);
    if (!patch::memory::IsReadable(slot, sizeof(*slot))) return 0;
    void* const manager = *slot;
    if (!manager || !patch::memory::IsReadable(manager, 1000u)) return 0;
    void* product = Field<void*>(manager, 996u);
    for (unsigned count = 0; product && count < 128u; ++count) {
        if (!patch::memory::IsReadable(product, 400u)) break;
        if (Field<void*>(product, 0u) ==
            g_imageBase + kTitleVisualVtableRva) {
            const int mode = Field<int>(product, 388u);
            if (mode == 2 || mode == 4 || mode == 5)
                g_cachedMode = mode;
            break;
        }
        product = Field<void*>(product, 8u);
    }
    return g_cachedMode;
}

// VFX drawing submits the model-space matrix after its unit matrices have
// already been built. The root picture includes the wallpaper, so correcting
// the GUI root would shrink the background alongside the logo. Instead scope
// individual draw calls to top-level game VFX parts.
const char* TopLevelName(void* unit) {
    if (!unit) return nullptr;
    void* part = unit;
    for (unsigned depth = 0; depth < 24u; ++depth) {
        void* const parent = Field<void*>(part, 4u);
        if (!parent) break;
        if (!Field<void*>(parent, 4u)) break;
        part = parent;
    }
    void* const resource = Field<void*>(part, 8u);
    void* const descriptor = resource ? Field<void*>(resource, 4u) : nullptr;
    return descriptor ? Field<const char*>(descriptor, 28u) : nullptr;
}

bool IsFullscreenEffect(const char* name) {
    if (!name) return true;
    // Shift-JIS prefixes from the game's own title VFX resources:
    // 壁紙 (wallpaper), キャプチャ (capture), ブラー (blur).
    constexpr char wallpaper[] = "\x95\xC7\x8E\x86";
    constexpr char capture[] = "\x83\x4C\x83\x83\x83\x76\x83\x60\x83\x83";
    constexpr char blur[] = "\x83\x75\x83\x89\x81\x5B";
    return std::strncmp(name, wallpaper, sizeof(wallpaper) - 1u) == 0 ||
           std::strncmp(name, capture, sizeof(capture) - 1u) == 0 ||
           std::strncmp(name, blur, sizeof(blur) - 1u) == 0;
}

int __fastcall VfxDrawHook(void* control, void*) {
    if (!control || !display::CurrentAspectRatio().correctionRequired ||
        !ActiveTitleMode()) return g_draw(control);
    void* const unit = Field<void*>(control, 8u);
    const char* const name = TopLevelName(unit);
    const bool previous = g_safeTitlePart;
    g_safeTitlePart = name && !IsFullscreenEffect(name);
    const int result = g_draw(control);
    g_safeTitlePart = previous;
    return result;
}

int __fastcall VfxGeometryDrawHook(void* model, void*, void* renderState,
                                   void* mesh, const float* matrix) {
    if (!g_safeTitlePart || !matrix)
        return g_geometryDraw(model, renderState, mesh, matrix);
    const auto aspect = display::CurrentAspectRatio();
    if (!aspect.valid || !aspect.correctionRequired)
        return g_geometryDraw(model, renderState, mesh, matrix);
    std::array<float, 16> corrected{};
    std::memcpy(corrected.data(), matrix, sizeof(corrected));
    for (const std::size_t index : {0u, 4u, 8u, 12u})
        corrected[index] *= aspect.safeScaleX;
    for (const std::size_t index : {1u, 5u, 9u, 13u})
        corrected[index] *= aspect.safeScaleY;
    // This matrix is centred VFX world space, not the GUI's top-left canvas.
    return g_geometryDraw(model, renderState, mesh, corrected.data());
}

} // namespace

bool Install(const ImageView& image) {
    const auto draw = image.At(kVfxDrawRva, kVfxDrawPrefix.size());
    const auto geometry = image.At(kVfxGeometryDrawRva,
                                   kVfxGeometryDrawPrefix.size());
    if (!draw || !geometry ||
        std::memcmp(draw->data(), kVfxDrawPrefix.data(),
                    kVfxDrawPrefix.size()) != 0 ||
        std::memcmp(geometry->data(), kVfxGeometryDrawPrefix.data(),
                    kVfxGeometryDrawPrefix.size()) != 0) return false;
    g_imageBase = const_cast<std::byte*>(image.Bytes().data());
    if (!g_drawDetour.Install(
            "ff13-2-title-vfx-draw-scope",
            const_cast<std::byte*>(draw->data()),
            reinterpret_cast<void*>(&VfxDrawHook), kVfxDrawPrefix,
            reinterpret_cast<void**>(&g_draw)) ||
        !g_geometryDetour.Install(
            "ff13-2-title-vfx-art-aspect",
            const_cast<std::byte*>(geometry->data()),
            reinterpret_cast<void*>(&VfxGeometryDrawHook),
            kVfxGeometryDrawPrefix,
            reinterpret_cast<void**>(&g_geometryDraw))) {
        Revert();
        return false;
    }
    return true;
}

void Revert() {
    g_geometryDetour.RevertAndClear(g_geometryDraw);
    g_drawDetour.RevertAndClear(g_draw);
    g_imageBase = nullptr;
}

} // namespace novafix::game::xiii2_ui::title_vfx_aspect
