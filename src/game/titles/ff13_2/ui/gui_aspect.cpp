#include "game/titles/ff13_2/ui/gui_aspect.h"

#include "game/titles/ff13_2/camera/aspect_ratio.h"

#include "d3d9/shaders/minimap_shader_variant.h"
#include "display/aspect_runtime.h"
#include "display/scissor_transform.h"
#include "game/core/image_view.h"
#include "game/titles/ff13_2/ui/crystarium_list.h"
#include "game/titles/ff13_2/ui/minimap_layout.h"
#include "patch/detour32.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"
#include "settings/graphics_settings.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_ui::gui_aspect {
namespace {

using xiii2_aspect_ratio::MinimapClipCorrection;

constexpr std::uint32_t kGuiMatrixUploadRva = 0x0039E570u;
constexpr std::uint32_t kGuiTaskDrawRva =
    0x00C09720u - 0x00400000u;
constexpr std::uint32_t kGuiScissorSubmitRva =
    0x00C1AD60u - 0x00400000u;
constexpr std::uint32_t kGuiNaviMapSpecialUploadReturnRva = 0x00490D22u;
constexpr std::uint32_t kGuiNativeAspectRva = 0x007A4B90u - 0x00400000u;
constexpr std::uint32_t kGuiCameraUploadRva = 0x0079C340u - 0x00400000u;
constexpr std::uint32_t kMinimapMatrixCallRva = 0x00890D1Du - 0x00400000u;
constexpr std::uint32_t kMinimapDrawRva = 0x00890AB0u - 0x00400000u;
constexpr std::array<std::uint32_t, 2> kMinimapAspectCallRvas{{
    0x00890B4Du - 0x00400000u,
    0x00890C69u - 0x00400000u,
}};
constexpr std::array<std::byte, 13> kGuiMatrixUploadPrefix{
    std::byte{0x56}, std::byte{0x8B}, std::byte{0xF1}, std::byte{0x8B},
    std::byte{0x46}, std::byte{0x08}, std::byte{0x57}, std::byte{0x8B},
    std::byte{0x7C}, std::byte{0x24}, std::byte{0x0C}, std::byte{0x85},
    std::byte{0xC0},
};
constexpr std::array<std::byte, 6> kGuiTaskDrawPrefix{
    std::byte{0x51}, std::byte{0x53}, std::byte{0x56},
    std::byte{0x57}, std::byte{0x8B}, std::byte{0xF9},
};
constexpr std::array<std::byte, 12> kMinimapDrawPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x83}, std::byte{0xE4}, std::byte{0xF0},
    std::byte{0x81}, std::byte{0xEC}, std::byte{0x44},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
};
constexpr std::array<std::byte, 15> kGuiScissorSubmitPrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x10}, std::byte{0x8B},
    std::byte{0x44}, std::byte{0x24}, std::byte{0x14}, std::byte{0x8B},
    std::byte{0x54}, std::byte{0x24}, std::byte{0x1C}, std::byte{0x8B},
    std::byte{0x4C}, std::byte{0x24}, std::byte{0x18},
};

using GuiMatrixUploadFn = int (__thiscall*)(
    void* shaderState, const float* matrix);
using GuiTaskDrawFn = char (__thiscall*)(void* task);
using MinimapDrawFn = int (__thiscall*)(void* product, int pass);
using GuiScissorSubmitFn = int (__cdecl*)(
    int x, int y, int width, int height);
using GuiNativeAspectFn = double (__cdecl*)();

patch::Detour32 g_guiMatrixUploadDetour;
patch::Detour32 g_guiTaskDrawDetour;
patch::Detour32 g_minimapDrawDetour;
patch::Detour32 g_guiScissorSubmitDetour;
GuiMatrixUploadFn g_originalGuiMatrixUpload{};
GuiTaskDrawFn g_originalGuiTaskDraw{};
MinimapDrawFn g_originalMinimapDraw{};
GuiScissorSubmitFn g_originalGuiScissorSubmit{};
GuiNativeAspectFn g_nativeGuiAspect{};
const std::byte* g_imageBase{};
thread_local unsigned g_guiTaskDrawDepth{};
thread_local MinimapClipCorrection g_minimapClipCorrection{};

std::byte* ImageTarget(const ImageView& image, std::uint32_t rva) {
    const auto bytes = image.At(rva, 1u);
    return bytes ? const_cast<std::byte*>(bytes->data()) : nullptr;
}

bool AddMinimapCall(patch::Transaction& calls, const ImageView& image,
                    std::uint32_t siteRva, std::uint32_t targetRva,
                    std::uintptr_t hook) {
    auto* site = ImageTarget(image, siteRva);
    auto* target = ImageTarget(image, targetRva);
    if (!site || !target) return false;
    const auto expected = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(site),
        reinterpret_cast<std::uintptr_t>(target));
    const auto replacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(site), hook);
    return expected && replacement &&
           calls.Add(siteRva, *expected, *replacement);
}

int __fastcall GuiMatrixUploadHook(
    void* shaderState, void*, const float* matrix) {
    const GuiMatrixUploadFn original = g_originalGuiMatrixUpload;
    if (!original) return 0;

    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (!matrix || !aspect.valid || !aspect.correctionRequired) {
        return original(shaderState, matrix);
    }

    const auto* returnAddress =
        static_cast<const std::byte*>(_ReturnAddress());
    if (g_imageBase &&
        returnAddress ==
            g_imageBase + kGuiNaviMapSpecialUploadReturnRva) {
        return original(shaderState, matrix);
    }

    std::array<float, 16> corrected{};
    std::memcpy(corrected.data(), matrix, sizeof(corrected));
    for (const std::size_t index : {0u, 4u, 8u, 12u}) {
        corrected[index] *= aspect.safeScaleX;
    }
    for (const std::size_t index : {1u, 5u, 9u, 13u}) {
        corrected[index] *= aspect.safeScaleY;
    }
    return original(shaderState, corrected.data());
}

double __cdecl MinimapAspectHook() {
    if (!d3d9::minimap_shader_variant::Available()) {
        return g_nativeGuiAspect();
    }
    const auto aspect = display::CurrentAspectRatio();
    if (aspect.valid && aspect.correctionRequired) {
        return xiii2_ui::MinimapCameraInverseAspect(aspect, 0.0f);
    }
    return g_nativeGuiAspect();
}

int __fastcall MinimapMatrixHook(
    void* camera, void*, const float* matrix) {
    const GuiMatrixUploadFn native = g_originalGuiMatrixUpload;
    if (!native) return 0;
    auto* shaderState = static_cast<std::byte*>(camera) + 0x20u;
    const auto aspect = display::CurrentAspectRatio();
    if (!matrix || !aspect.valid ||
        !d3d9::minimap_shader_variant::Available()) {
        g_minimapClipCorrection.active = false;
        return native(shaderState, matrix);
    }

    const auto graphics = settings::Active();
    if (!aspect.correctionRequired && !graphics.roundMinimap &&
        graphics.minimapScale == 1.0f &&
        graphics.minimapHorizontalOffset == 0.0f &&
        graphics.minimapVerticalOffset == 0.0f) {
        g_minimapClipCorrection.active = false;
        return native(shaderState, matrix);
    }

    const auto layout = xiii2_ui::ResolveVisibleMinimapLayout(
        aspect, graphics.minimapScale, graphics.roundMinimap,
        graphics.minimapHorizontalOffset,
        graphics.minimapVerticalOffset);
    std::array<float, 16> corrected{};
    std::memcpy(corrected.data(), matrix, sizeof(corrected));
    if (aspect.correctionRequired) {
        for (const std::size_t index : {0u, 4u, 8u, 12u}) {
            corrected[index] *= aspect.safeScaleX;
        }
        for (const std::size_t index : {1u, 5u, 9u, 13u}) {
            corrected[index] *= aspect.safeScaleY;
        }
    }
    if (layout.changed) xiii2_ui::ApplyMinimapLayout(corrected, layout);
    if (g_minimapClipCorrection.active) {
        const auto fill = xiii2_ui::MinimapFillAndClipY(
            layout, graphics.roundMinimap);
        const auto inverse = xiii2_ui::MinimapClipCoordinateInverse(
            aspect, layout);
        std::copy(fill.begin(), fill.end(),
                  g_minimapClipCorrection.constants.begin());
        std::copy(inverse.begin(), inverse.end(),
                  g_minimapClipCorrection.constants.begin() + 4);
    }
    return native(shaderState, corrected.data());
}

int __fastcall MinimapDrawHook(void* product, void*, int pass) {
    const MinimapDrawFn original = g_originalMinimapDraw;
    if (!original) return 0;
    if (xiii2_ui::MinimapHidden(settings::Active().minimapScale)) return 0;
    struct RestoreClipCorrection {
        MinimapClipCorrection previous{g_minimapClipCorrection};
        ~RestoreClipCorrection() { g_minimapClipCorrection = previous; }
    } restore;
    g_minimapClipCorrection = {};
    g_minimapClipCorrection.active = true;
    return original(product, pass);
}

char __fastcall GuiTaskDrawHook(void* task, void*) {
    const GuiTaskDrawFn original = g_originalGuiTaskDraw;
    if (!original) return 0;

    struct ScopedGuiTaskDraw {
        ScopedGuiTaskDraw() { ++g_guiTaskDrawDepth; }
        ~ScopedGuiTaskDraw() { --g_guiTaskDrawDepth; }
    } scope;
    return original(task);
}

int __cdecl GuiScissorSubmitHook(
    int x, int y, int width, int height) {
    const GuiScissorSubmitFn original = g_originalGuiScissorSubmit;
    if (!original) return 0;

    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (g_guiTaskDrawDepth == 0u || !aspect.valid ||
        !aspect.correctionRequired || width < 0 || height < 0) {
        return original(x, y, width, height);
    }

    const std::int64_t right = static_cast<std::int64_t>(x) + width;
    const std::int64_t bottom = static_cast<std::int64_t>(y) + height;
    if (right > INT32_MAX || bottom > INT32_MAX) {
        return original(x, y, width, height);
    }
    display::ScissorRect corrected =
        display::FitScissorRectToSafeArea(
            {x, y, static_cast<std::int32_t>(right),
             static_cast<std::int32_t>(bottom)},
            aspect);
    corrected = xiii2_crystarium_list::CorrectScissor(
        {x, y, static_cast<std::int32_t>(right),
         static_cast<std::int32_t>(bottom)}, corrected, aspect);

    return original(
        corrected.left, corrected.top,
        corrected.right - corrected.left,
        corrected.bottom - corrected.top);
}

} // namespace

bool MatchesContract(const ImageView& image) {
    const auto minimapDraw = image.At(
        kMinimapDrawRva, kMinimapDrawPrefix.size());
    const auto guiMatrixUpload = image.At(
        kGuiMatrixUploadRva, kGuiMatrixUploadPrefix.size());
    const auto guiTaskDraw = image.At(
        kGuiTaskDrawRva, kGuiTaskDrawPrefix.size());
    const auto guiScissorSubmit = image.At(
        kGuiScissorSubmitRva, kGuiScissorSubmitPrefix.size());
    return minimapDraw && guiMatrixUpload && guiTaskDraw && guiScissorSubmit &&
           std::memcmp(minimapDraw->data(), kMinimapDrawPrefix.data(),
                       kMinimapDrawPrefix.size()) == 0 &&
           std::memcmp(guiMatrixUpload->data(), kGuiMatrixUploadPrefix.data(),
                       kGuiMatrixUploadPrefix.size()) == 0 &&
           std::memcmp(guiTaskDraw->data(), kGuiTaskDrawPrefix.data(),
                       kGuiTaskDrawPrefix.size()) == 0 &&
           std::memcmp(guiScissorSubmit->data(),
                       kGuiScissorSubmitPrefix.data(),
                       kGuiScissorSubmitPrefix.size()) == 0;
}

void Prepare(const ImageView& image) {
    g_imageBase = image.Bytes().data();
}

void PrepareNativeAspect(const ImageView& image) {
    g_nativeGuiAspect = reinterpret_cast<GuiNativeAspectFn>(
        ImageTarget(image, kGuiNativeAspectRva));
}

void ResetPreparedState() {
    g_nativeGuiAspect = nullptr;
    g_imageBase = nullptr;
}

bool RegisterMinimapAspectCalls(patch::Transaction& calls,
                                const ImageView& image) {
    for (const auto site : kMinimapAspectCallRvas) {
        if (!AddMinimapCall(calls, image, site, kGuiNativeAspectRva,
                reinterpret_cast<std::uintptr_t>(&MinimapAspectHook))) {
            return false;
        }
    }
    return true;
}

bool RegisterMinimapMatrixCall(patch::Transaction& calls,
                               const ImageView& image) {
    return AddMinimapCall(calls, image, kMinimapMatrixCallRva,
        kGuiCameraUploadRva,
        reinterpret_cast<std::uintptr_t>(&MinimapMatrixHook));
}

bool InstallDetours(const ImageView& image) {
    const auto minimapDraw = image.At(
        kMinimapDrawRva, kMinimapDrawPrefix.size());
    const auto guiMatrixUpload = image.At(
        kGuiMatrixUploadRva, kGuiMatrixUploadPrefix.size());
    const auto guiTaskDraw = image.At(
        kGuiTaskDrawRva, kGuiTaskDrawPrefix.size());
    const auto guiScissorSubmit = image.At(
        kGuiScissorSubmitRva, kGuiScissorSubmitPrefix.size());
    if (!minimapDraw || !guiMatrixUpload ||
        !guiTaskDraw || !guiScissorSubmit) return false;

    return g_minimapDrawDetour.Install(
            "ff13-2-minimap-draw-scope",
            const_cast<std::byte*>(minimapDraw->data()),
            reinterpret_cast<void*>(&MinimapDrawHook),
            kMinimapDrawPrefix,
            reinterpret_cast<void**>(&g_originalMinimapDraw)) &&
        g_guiMatrixUploadDetour.Install(
            "ff13-2-gui-matrix-safe-area",
            const_cast<std::byte*>(guiMatrixUpload->data()),
            reinterpret_cast<void*>(&GuiMatrixUploadHook),
            kGuiMatrixUploadPrefix,
            reinterpret_cast<void**>(&g_originalGuiMatrixUpload)) &&
        g_guiTaskDrawDetour.Install(
            "ff13-2-gui-task-aspect-scope",
            const_cast<std::byte*>(guiTaskDraw->data()),
            reinterpret_cast<void*>(&GuiTaskDrawHook),
            kGuiTaskDrawPrefix,
            reinterpret_cast<void**>(&g_originalGuiTaskDraw)) &&
        g_guiScissorSubmitDetour.Install(
            "ff13-2-gui-scissor-safe-area",
            const_cast<std::byte*>(guiScissorSubmit->data()),
            reinterpret_cast<void*>(&GuiScissorSubmitHook),
            kGuiScissorSubmitPrefix,
            reinterpret_cast<void**>(&g_originalGuiScissorSubmit));
}

void RevertMinimapDraw() {
    g_minimapDrawDetour.RevertAndClear(g_originalMinimapDraw);
}

void RevertGui() {
    g_guiScissorSubmitDetour.RevertAndClear(g_originalGuiScissorSubmit);
    g_guiTaskDrawDetour.RevertAndClear(g_originalGuiTaskDraw);
    g_guiMatrixUploadDetour.RevertAndClear(g_originalGuiMatrixUpload);
}

xiii2_aspect_ratio::MinimapClipCorrection CurrentMinimapClipCorrection() {
    return g_minimapClipCorrection;
}

} // namespace novafix::game::xiii2_ui::gui_aspect
