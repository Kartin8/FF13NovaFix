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

patch::Detour32 g_guiMatrixDetour;
patch::Detour32 g_guiDrawDetour;
patch::Detour32 g_minimapDrawDetour;
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
            reinterpret_cast<void**>(&g_minimapDraw));
}

void Revert() {
    g_guiDrawDetour.RevertAndClear(g_guiDraw);
    g_minimapDrawDetour.RevertAndClear(g_minimapDraw);
    g_guiMatrixDetour.RevertAndClear(g_guiMatrix);
}

} // namespace novafix::game::ff13_ui::gui_aspect
