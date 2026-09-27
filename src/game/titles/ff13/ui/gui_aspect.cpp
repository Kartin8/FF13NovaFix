#include "game/titles/ff13/ui/gui_aspect.h"

#include "display/aspect_runtime.h"
#include "game/core/image_view.h"
#include "game/titles/ff13/camera/aspect_ratio_internal.h"
#include "game/titles/ff13/ui/minimap_aspect.h"
#include "game/titles/ff13/ui/minimap_layout.h"
#include "game/titles/ff13/ui/minimap_tilt.h"
#include "patch/detour32.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"
#include "settings/graphics_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace novafix::game::ff13_aspect_ratio {
thread_local unsigned g_guiDrawDepth{};
}

namespace novafix::game::ff13_ui::gui_aspect {
namespace {

using ff13_aspect_ratio::g_guiDrawDepth;

constexpr std::uint32_t kGuiMatrixRva = 0x8FB590u - 0x400000u;
constexpr std::uint32_t kGuiDrawRva = 0xE988D0u - 0x400000u;
constexpr std::uint32_t kGuiAspectRva = 0x905FB0u - 0x400000u;
constexpr std::uint32_t kMinimapDrawRva = 0xA05290u - 0x400000u;
constexpr std::uint32_t kGuiCameraRva = 0x8F9210u - 0x400000u;
constexpr std::uint32_t kMinimapBeginCameraCallRva = 0xA0584Cu - 0x400000u;
constexpr std::uint32_t kMinimapEndCameraCallRva = 0xA0606Fu - 0x400000u;
constexpr std::size_t kMinimapCenterXOffset = 1992u * sizeof(int);
constexpr std::size_t kMinimapCenterYOffset = 1993u * sizeof(int);
constexpr std::array<std::uint32_t, 2> kMinimapAspectCallRvas{{
    0xA0546Cu - 0x400000u,
    0xA05677u - 0x400000u,
}};
constexpr std::uint32_t kGuiProjectionRva = 0x905980u - 0x400000u;
constexpr std::uint32_t kGuiViewRva = 0x905B80u - 0x400000u;
constexpr std::uint32_t kGuiProjectionCallRva = 0x905883u - 0x400000u;
constexpr std::uint32_t kGuiViewCallRva = 0x9058A0u - 0x400000u;
constexpr std::uint32_t kTitleWallpaperMatrixRva = 0xCD4620u - 0x400000u;
constexpr std::uint32_t kTitleDrawCallRva = 0xCC4F20u - 0x400000u;
constexpr std::uint32_t kTitleGeometryDrawRva = 0xC97270u - 0x400000u;
constexpr std::uint32_t kGuiManagerPointerRva = 0x2826A4Cu - 0x400000u;
constexpr std::uint32_t kTitleLogoVtableRva = 0x11997B0u - 0x400000u;
constexpr std::array<std::byte, 5> kTitleWallpaperMatrixPrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x68},
    std::byte{0x56}, std::byte{0x8B}};
constexpr std::array<std::byte, 6> kTitleDrawCallPrefix{
    std::byte{0x56}, std::byte{0x8B}, std::byte{0xF1},
    std::byte{0x8B}, std::byte{0x4E}, std::byte{0x04}};
constexpr std::array<std::byte, 6> kTitleGeometryDrawPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x08}};
constexpr std::array<std::byte, 9> kGuiMatrixPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x0C}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xF4}};
constexpr std::array<std::byte, 9> kGuiDrawPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x54}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xB4}};
constexpr std::array<std::byte, 12> kMinimapDrawPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xE4}, std::byte{0xF0}, std::byte{0x81}, std::byte{0xEC},
    std::byte{0xB0}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}};
constexpr std::array<std::byte, 5> kGuiProjectionCall{
    std::byte{0xE8}, std::byte{0xF8}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}};
constexpr std::array<std::byte, 5> kGuiViewCall{
    std::byte{0xE8}, std::byte{0xDB}, std::byte{0x02}, std::byte{0x00},
    std::byte{0x00}};

using GuiMatrixFn = int (__thiscall*)(void*, const float*);
using GuiDrawFn = int (__thiscall*)(void*);
using GuiAspectFn = double (__cdecl*)();
using MinimapDrawFn = void* (__thiscall*)(void*, int);
using GuiCameraFn = int (__thiscall*)(void*, const float*);
using GuiTransformFn = std::uintptr_t (__cdecl*)(float*, float, float);
using VfxMatrixFn = void* (__thiscall*)(void*, void*);
using VfxDrawFn = int (__thiscall*)(void*);
using VfxGeometryDrawFn = int (__thiscall*)(void*, void*, void*, const float*);

patch::Detour32 g_guiMatrixDetour;
patch::Detour32 g_guiDrawDetour;
patch::Detour32 g_minimapDrawDetour;
patch::Detour32 g_titleWallpaperDetour;
patch::Detour32 g_titleDrawDetour;
patch::Detour32 g_titleGeometryDetour;
VfxMatrixFn g_titleWallpaperMatrix{};
VfxDrawFn g_titleDrawCall{};
VfxGeometryDrawFn g_titleGeometryDraw{};
void** g_guiManagerPointer{};
void* g_titleLogoVtable{};
GuiMatrixFn g_guiMatrix{};
GuiDrawFn g_guiDraw{};
GuiAspectFn g_guiAspect{};
MinimapDrawFn g_minimapDraw{};
GuiCameraFn g_guiCamera{};
GuiTransformFn g_guiProjection{};
GuiTransformFn g_guiView{};

struct MinimapDrawLayoutContext {
    bool prepared{};
    bool active{};
    ff13_ui::MinimapLayout layout{};
};
thread_local MinimapDrawLayoutContext g_minimapLayout{};

template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

enum class TitleLayer {
    Wallpaper,
    DarkWallpaper,
    Crest,
    GreyCrest,
    LineFrame,
    Line,
    Letters,
    LetterOutlines,
    Other,
    Count,
};

bool IsTitleVfxVariant(int variant) {
    return variant == 2 || variant == 4 || variant == 5;
}

std::atomic<void*> g_normalTitleRoot{};
std::atomic<void*> g_greyTitleRoot{};
thread_local TitleLayer g_activeTitleDrawLayer{TitleLayer::Count};
thread_local void* g_activeTitleDrawRoot{};

const char* UnitName(void* unit) {
    if (!unit) return nullptr;
    void* const resource = Field<void*>(unit, 8u);
    void* const descriptor = resource ? Field<void*>(resource, 4u) : nullptr;
    return descriptor ? Field<const char*>(descriptor, 28u) : nullptr;
}

struct TitleUnitInfo {
    TitleLayer layer{TitleLayer::Count};
    void* root{};
};

TitleUnitInfo LayerForUnit(void* unit) {
    void* child = unit;
    for (unsigned depth = 0; child && depth < 16u; ++depth) {
        void* const parent = Field<void*>(child, 4u);
        const char* const parentName = UnitName(parent);
        if (!parent || !parentName) break;
        if (std::strcmp(parentName, "Root") == 0) {
            const char* const name = UnitName(child);
            if (!name) break;
            constexpr char wallpaper[] = "\x95\xC7\x8E\x86\x81\x51\x94\x92";
            constexpr char darkWallpaper[] = "\x95\xC7\x8E\x86\x81\x51\x8D\x95";
            constexpr char crest[] = "\x83\x8D\x83\x53";
            constexpr char greyCrest[] =
                "\x83\x8D\x83\x53(\x83\x4F\x83\x8C\x83\x43\x81\x6A";
            constexpr char letters[] = "\x83\x8D\x83\x53\x8C\xC2\x95\xCA";
            if (std::strcmp(name, wallpaper) == 0) {
                return {TitleLayer::Wallpaper, parent};
            }
            if (std::strcmp(name, darkWallpaper) == 0) {
                return {TitleLayer::DarkWallpaper, parent};
            }
            if (std::strcmp(name, crest) == 0) {
                g_normalTitleRoot.store(parent, std::memory_order_relaxed);
                return {TitleLayer::Crest, parent};
            }
            if (std::strcmp(name, greyCrest) == 0) {
                g_greyTitleRoot.store(parent, std::memory_order_relaxed);
                return {TitleLayer::GreyCrest, parent};
            }
            if (std::strcmp(name, "LINE waku") == 0) {
                return {TitleLayer::LineFrame, parent};
            }
            if (std::strcmp(name, "LINE") == 0) {
                return {TitleLayer::Line, parent};
            }
            if (std::strncmp(name, letters, sizeof(letters) - 1u) == 0) {
                return name[sizeof(letters) - 1u] == '\0'
                    ? TitleUnitInfo{TitleLayer::Letters, parent}
                    : TitleUnitInfo{TitleLayer::LetterOutlines, parent};
            }
            return {TitleLayer::Other, parent};
        }
        child = parent;
    }
    return {};
}

int TitleLogoMode() {
    if (!g_guiManagerPointer || !g_titleLogoVtable) return 0;
    void* const manager = *g_guiManagerPointer;
    if (!manager) return 0;
    void* product = Field<void*>(manager, 712u);
    for (unsigned count = 0; product && count < 32u; ++count) {
        if (Field<void*>(product, 0u) == g_titleLogoVtable) {
            return Field<int>(product, 0x184u);
        }
        product = Field<void*>(product, 8u);
    }
    return 0;
}

bool TitleLogoActive() {
    return IsTitleVfxVariant(TitleLogoMode());
}

void* __fastcall TitleWallpaperMatrixHook(void* unitMatrix, void*,
                                          void* unit) {
    void* const result = g_titleWallpaperMatrix(unitMatrix, unit);
    const auto aspect = display::CurrentAspectRatio();
    if (!result || !unit || !unitMatrix || !aspect.valid ||
        !aspect.correctionRequired) return result;
    if (unitMatrix != static_cast<std::byte*>(unit) + 0xACu) return result;
    const auto layer = LayerForUnit(unit).layer;
    if (layer == TitleLayer::Count || !TitleLogoActive()) return result;
    auto* const matrix = static_cast<float*>(unitMatrix);
    if (layer == TitleLayer::Wallpaper && aspect.correctionRequired &&
        aspect.safeScaleX > 0.0f && aspect.safeScaleY > 0.0f) {
        for (const std::size_t index : {0u, 4u, 8u}) {
            matrix[index] /= aspect.safeScaleX;
        }
        for (const std::size_t index : {1u, 5u, 9u}) {
            matrix[index] /= aspect.safeScaleY;
        }
    }
    return result;
}

int __fastcall TitleDrawHook(void* drawControl, void*) {
    if (!drawControl ||
        !display::CurrentAspectRatio().correctionRequired)
        return g_titleDrawCall(drawControl);
    void* const unit = Field<void*>(drawControl, 8u);
    const auto info = LayerForUnit(unit);
    const auto layer = info.layer;
    if (layer == TitleLayer::Count || !TitleLogoActive()) {
        return g_titleDrawCall(drawControl);
    }
    const auto previous = g_activeTitleDrawLayer;
    void* const previousRoot = g_activeTitleDrawRoot;
    g_activeTitleDrawLayer = layer;
    g_activeTitleDrawRoot = info.root;
    const int result = g_titleDrawCall(drawControl);
    g_activeTitleDrawLayer = previous;
    g_activeTitleDrawRoot = previousRoot;
    return result;
}

void ApplyTitleAspect(float* matrix, float scaleX, float scaleY) {
    for (const std::size_t index : {0u, 4u, 8u}) matrix[index] *= scaleX;
    for (const std::size_t index : {1u, 5u, 9u}) matrix[index] *= scaleY;
    // Submitted VFX geometry is centred world space, not the GUI's
    // top-left-origin 1280x720 canvas. Scale translation about world zero.
    matrix[12] *= scaleX;
    matrix[13] *= scaleY;
}

int __fastcall TitleGeometryDrawHook(void* model, void*, void* renderState,
                                     void* mesh, const float* matrix) {
    const auto aspect = display::CurrentAspectRatio();
    if (!matrix || !aspect.valid || !aspect.correctionRequired)
        return g_titleGeometryDraw(model, renderState, mesh, matrix);
    const auto layer = g_activeTitleDrawLayer;
    if (layer == TitleLayer::Count) {
        return g_titleGeometryDraw(model, renderState, mesh, matrix);
    }
    const bool aspectValid =
        aspect.safeScaleX > 0.0f && aspect.safeScaleY > 0.0f;
    const bool normalVfx = g_activeTitleDrawRoot &&
        g_activeTitleDrawRoot ==
            g_normalTitleRoot.load(std::memory_order_relaxed);
    const bool introVfx = TitleLogoMode() == 2;
    const bool wallpaper = layer == TitleLayer::Wallpaper;
    const bool introBackdrop = introVfx &&
        (wallpaper || layer == TitleLayer::DarkWallpaper);
    // The intro's VFX root differs from the interactive colour title.
    // Its backdrops still inherit the same safe-area shrink from the model.
    const bool fullWallpaper = aspectValid &&
        ((normalVfx && wallpaper) || introBackdrop);
    const bool safeGreyVfx = aspectValid &&
        g_activeTitleDrawRoot ==
            g_greyTitleRoot.load(std::memory_order_relaxed) &&
        g_activeTitleDrawRoot &&
        layer != TitleLayer::Wallpaper &&
        layer != TitleLayer::DarkWallpaper &&
        layer != TitleLayer::Other;
    if (!fullWallpaper && !safeGreyVfx) {
        return g_titleGeometryDraw(model, renderState, mesh, matrix);
    }
    std::array<float, 16> adjusted{};
    std::memcpy(adjusted.data(), matrix, sizeof(adjusted));
    if (fullWallpaper) {
        ApplyTitleAspect(adjusted.data(), 1.0f / aspect.safeScaleX,
                         1.0f / aspect.safeScaleY);
    } else if (safeGreyVfx) {
        ApplyTitleAspect(adjusted.data(), aspect.safeScaleX,
                         aspect.safeScaleY);
    }
    return g_titleGeometryDraw(model, renderState, mesh, adjusted.data());
}

void* Target(const ImageView& image, std::uint32_t rva) {
    const auto range = image.At(rva, 1u);
    return range ? const_cast<std::byte*>(range->data()) : nullptr;
}

int __fastcall GuiMatrixHook(void* shader, void*, const float* matrix) {
    const auto aspect = display::CurrentAspectRatio();
    const bool safeArea = g_guiDrawDepth != 0u && matrix && aspect.valid &&
        aspect.correctionRequired;
    const bool minimap = g_minimapLayout.active && matrix;
    if (!safeArea && !minimap) {
        return g_guiMatrix(shader, matrix);
    }
    std::array<float, 16> corrected{};
    std::memcpy(corrected.data(), matrix, sizeof(corrected));
    if (safeArea) {
        for (const std::size_t index : {0u, 4u, 8u, 12u}) {
            corrected[index] *= aspect.safeScaleX;
        }
        for (const std::size_t index : {1u, 5u, 9u, 13u}) {
            corrected[index] *= aspect.safeScaleY;
        }
    }
    if (minimap) {
        const auto& layout = g_minimapLayout.layout;
        if (layout.changed) {
            ff13_ui::ApplyMinimapLayout(corrected, layout);
        }
    }
    return g_guiMatrix(shader, corrected.data());
}

int __fastcall GuiDrawHook(void* task, void*) {
    struct ScopedDraw {
        ScopedDraw() { ++g_guiDrawDepth; }
        ~ScopedDraw() { --g_guiDrawDepth; }
    } scope;
    return g_guiDraw(task);
}

double __cdecl MinimapAspectHook() {
    const auto aspect = display::CurrentAspectRatio();
    if (!aspect.valid || !aspect.correctionRequired) return g_guiAspect();
    return ff13_ui::MinimapCameraInverseAspect(aspect, 0.0f);
}

void* __fastcall MinimapDrawHook(void* product, void*, int pass) {
    if (ff13_ui::MinimapHidden(settings::Active().minimapScale)) return nullptr;
    struct RestoreContext {
        MinimapDrawLayoutContext previous{g_minimapLayout};
        ~RestoreContext() { g_minimapLayout = previous; }
    } restore;
    g_minimapLayout = {};
    const auto aspect = display::CurrentAspectRatio();
    const auto graphics = settings::Active();
    if (product && aspect.valid) {
        // A00DC0 writes these authored map-centre coordinates. The first
        // shader matrix's translation is a camera term, not the visible centre.
        g_minimapLayout.layout = ff13_ui::ResolveVisibleMinimapLayout(
            aspect, graphics.minimapScale,
            graphics.minimapHorizontalOffset,
            graphics.minimapVerticalOffset,
            static_cast<float>(Field<int>(product, kMinimapCenterXOffset)),
            static_cast<float>(Field<int>(product, kMinimapCenterYOffset)));
        g_minimapLayout.prepared = g_minimapLayout.layout.changed;
    }
    const ff13_ui::ScopedMinimapTilt tilt(
        product, settings::RoundMinimapEnabled());
    return g_minimapDraw(product, pass);
}

int __fastcall MinimapBeginCameraHook(void* camera, void*, const float* matrix) {
    g_minimapLayout.active = g_minimapLayout.prepared;
    return g_guiCamera(camera, matrix);
}

int __fastcall MinimapEndCameraHook(void* camera, void*, const float* matrix) {
    g_minimapLayout.active = false;
    return g_guiCamera(camera, matrix);
}

float GuiReferenceAspect(float original) {
    const auto aspect = display::CurrentAspectRatio();
    return g_guiDrawDepth != 0u && aspect.valid && aspect.correctionRequired
        ? 1.0f / display::kAuthoredAspectRatio : original;
}

std::uintptr_t __cdecl GuiProjectionHook(float* output, float aspect,
                                        float fov) {
    return g_guiProjection(output, GuiReferenceAspect(aspect), fov);
}

std::uintptr_t __cdecl GuiViewHook(float* output, float aspect, float fov) {
    return g_guiView(output, GuiReferenceAspect(aspect), fov);
}

} // namespace

bool RegisterMinimapAspectCalls(patch::Transaction& calls,
                                const ImageView& image) {
    for (const auto rva : kMinimapAspectCallRvas) {
        const auto expected = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(Target(image, rva)),
            reinterpret_cast<std::uintptr_t>(Target(image, kGuiAspectRva)));
        const auto replacement = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(Target(image, rva)),
            reinterpret_cast<std::uintptr_t>(&MinimapAspectHook));
        if (!expected || !replacement ||
            !calls.Add(rva, *expected, *replacement)) return false;
    }
    return true;
}

bool RegisterMinimapCameraCalls(patch::Transaction& calls,
                                const ImageView& image) {
    for (const auto site : std::array{
             std::pair{kMinimapBeginCameraCallRva,
                       reinterpret_cast<std::uintptr_t>(&MinimapBeginCameraHook)},
             std::pair{kMinimapEndCameraCallRva,
                       reinterpret_cast<std::uintptr_t>(&MinimapEndCameraHook)}}) {
        const auto expected = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(Target(image, site.first)),
            reinterpret_cast<std::uintptr_t>(Target(image, kGuiCameraRva)));
        const auto replacement = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(Target(image, site.first)),
            site.second);
        if (!expected || !replacement ||
            !calls.Add(site.first, *expected, *replacement)) return false;
    }
    return true;
}

bool RegisterProjectionCalls(patch::Transaction& calls,
                             const ImageView& image) {
    for (const auto site : std::array{
             std::pair{kGuiProjectionCallRva,
                       std::span<const std::byte>(kGuiProjectionCall)},
             std::pair{kGuiViewCallRva,
                       std::span<const std::byte>(kGuiViewCall)}}) {
        const auto hook = site.first == kGuiProjectionCallRva
            ? reinterpret_cast<std::uintptr_t>(&GuiProjectionHook)
            : reinterpret_cast<std::uintptr_t>(&GuiViewHook);
        const auto replacement = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(Target(image, site.first)), hook);
        if (!replacement ||
            !calls.Add(site.first, site.second, *replacement)) return false;
    }
    return true;
}

void Prepare(const ImageView& image) {
    g_guiManagerPointer = reinterpret_cast<void**>(
        Target(image, kGuiManagerPointerRva));
    g_titleLogoVtable = Target(image, kTitleLogoVtableRva);
    g_guiProjection =
        reinterpret_cast<GuiTransformFn>(Target(image, kGuiProjectionRva));
    g_guiAspect = reinterpret_cast<GuiAspectFn>(
        Target(image, kGuiAspectRva));
    g_guiCamera = reinterpret_cast<GuiCameraFn>(
        Target(image, kGuiCameraRva));
    g_guiView = reinterpret_cast<GuiTransformFn>(Target(image, kGuiViewRva));
}

bool InstallDetours(const ImageView& image) {
    return g_guiMatrixDetour.Install(
            "ff13-gui-aspect", Target(image, kGuiMatrixRva),
            reinterpret_cast<void*>(&GuiMatrixHook), kGuiMatrixPrefix,
            reinterpret_cast<void**>(&g_guiMatrix)) &&
        g_guiDrawDetour.Install(
            "ff13-gui-draw-scope", Target(image, kGuiDrawRva),
            reinterpret_cast<void*>(&GuiDrawHook), kGuiDrawPrefix,
            reinterpret_cast<void**>(&g_guiDraw)) &&
        g_minimapDrawDetour.Install(
            "ff13-round-minimap", Target(image, kMinimapDrawRva),
            reinterpret_cast<void*>(&MinimapDrawHook), kMinimapDrawPrefix,
            reinterpret_cast<void**>(&g_minimapDraw)) &&
        g_titleWallpaperDetour.Install(
            "ff13-title-wallpaper", Target(image, kTitleWallpaperMatrixRva),
            reinterpret_cast<void*>(&TitleWallpaperMatrixHook),
            kTitleWallpaperMatrixPrefix,
            reinterpret_cast<void**>(&g_titleWallpaperMatrix)) &&
        g_titleDrawDetour.Install(
            "ff13-title-draw-call", Target(image, kTitleDrawCallRva),
            reinterpret_cast<void*>(&TitleDrawHook),
            kTitleDrawCallPrefix,
            reinterpret_cast<void**>(&g_titleDrawCall)) &&
        g_titleGeometryDetour.Install(
            "ff13-title-geometry-draw", Target(image, kTitleGeometryDrawRva),
            reinterpret_cast<void*>(&TitleGeometryDrawHook),
            kTitleGeometryDrawPrefix,
            reinterpret_cast<void**>(&g_titleGeometryDraw));
}

void Revert() {
    g_titleGeometryDetour.RevertAndClear(g_titleGeometryDraw);
    g_titleDrawDetour.RevertAndClear(g_titleDrawCall);
    g_titleWallpaperDetour.RevertAndClear(g_titleWallpaperMatrix);
    g_guiDrawDetour.RevertAndClear(g_guiDraw);
    g_minimapDrawDetour.RevertAndClear(g_minimapDraw);
    g_guiMatrixDetour.RevertAndClear(g_guiMatrix);
}

} // namespace novafix::game::ff13_ui::gui_aspect
