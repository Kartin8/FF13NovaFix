#include "game/titles/ff13_2/camera/aspect_ratio.h"

#include "diagnostics/log.h"
#include "display/aspect_runtime.h"
#include "display/scissor_transform.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"

#include <windows.h>

#include <intrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace novafix::game::xiii2_aspect_ratio {
namespace {

constexpr std::uint32_t kFieldCameraLoadRva = 0x001996A0u;
constexpr std::uint32_t kSceneCameraDefaultsRva = 0x0026B970u;
constexpr std::uint32_t kSceneCameraPerspectiveRva = 0x00269CD0u;
constexpr std::uint32_t kVfxCameraProjectionUpdateRva = 0x006E8C20u;
constexpr std::uint32_t kGuiMatrixUploadRva = 0x0039E570u;
// DRAW_MENU task 68 brackets the complete White HUD/menu submission.
constexpr std::uint32_t kGuiTaskDrawRva =
    0x00C09720u - 0x00400000u;
// Both retained and explicit White clip paths converge on this submitter.
constexpr std::uint32_t kGuiScissorSubmitRva =
    0x00C1AD60u - 0x00400000u;
constexpr std::uint32_t kFinalCompositeModeRva = 0x00809FDBu;
constexpr std::uint32_t kMoviePresentRva = 0x0061F0C0u;
constexpr std::uint32_t kMoviePrimitiveCallRva = 0x008250D9u;
constexpr std::uint32_t kNativePrimitiveSubmitRva = 0x00824A60u;
constexpr std::uint32_t kGuiNaviMapSpecialUploadReturnRva = 0x00490D22u;
constexpr std::size_t kFieldAspectOffset = 0x0A1Cu;
constexpr std::size_t kSceneCameraAspectOffset = 0x30u;
constexpr std::size_t kVfxCameraAspectOffset = 0x138u;
constexpr std::size_t kMovieWidthOffset = 268u;
constexpr std::size_t kMovieHeightOffset = 272u;

constexpr std::array<std::byte, 13> kFieldCameraLoadPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xE4}, std::byte{0xF0}, std::byte{0x83}, std::byte{0xEC},
    std::byte{0x14}, std::byte{0x53}, std::byte{0x56}, std::byte{0x8B},
    std::byte{0xF1},
};
// Stop before the relocated absolute operand of `movss xmm1, [address]`.
constexpr std::array<std::byte, 9> kSceneCameraDefaultsPrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x10}, std::byte{0x0F},
    std::byte{0x57}, std::byte{0xC0}, std::byte{0xF3}, std::byte{0x0F},
    std::byte{0x10},
};
constexpr std::array<std::byte, 16> kSceneCameraPerspectivePrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x08}, std::byte{0x56},
    std::byte{0x57}, std::byte{0x8B}, std::byte{0xF9}, std::byte{0xE8},
    std::byte{0x74}, std::byte{0x95}, std::byte{0x01}, std::byte{0x00},
    std::byte{0x8B}, std::byte{0xF0}, std::byte{0x23}, std::byte{0x35},
};
constexpr std::array<std::byte, 16> kVfxCameraProjectionUpdatePrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x40}, std::byte{0x56},
    std::byte{0x8B}, std::byte{0xF1}, std::byte{0x0F}, std::byte{0xB7},
    std::byte{0x86}, std::byte{0x54}, std::byte{0x01}, std::byte{0x00},
    std::byte{0x00}, std::byte{0xA9}, std::byte{0x00}, std::byte{0x20},
};
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
constexpr std::array<std::byte, 15> kGuiScissorSubmitPrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x10}, std::byte{0x8B},
    std::byte{0x44}, std::byte{0x24}, std::byte{0x14}, std::byte{0x8B},
    std::byte{0x54}, std::byte{0x24}, std::byte{0x1C}, std::byte{0x8B},
    std::byte{0x4C}, std::byte{0x24}, std::byte{0x18},
};
constexpr std::array<std::byte, 12> kMoviePresentPrefix{
    std::byte{0x56}, std::byte{0x57}, std::byte{0x8B}, std::byte{0xF1},
    std::byte{0x8D}, std::byte{0xBE}, std::byte{0xB0}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x57}, std::byte{0xFF},
};
constexpr std::array<std::byte, 5> kMoviePrimitiveCall{
    std::byte{0xE8}, std::byte{0x82}, std::byte{0xF9}, std::byte{0xFF},
    std::byte{0xFF},
};
constexpr std::array<std::byte, 3> kFinalCompositeModeLoad{
    std::byte{0x8B}, std::byte{0x78}, std::byte{0x30},
};
constexpr std::array<std::byte, 3> kFullScreenCompositeMode{
    std::byte{0x33}, std::byte{0xFF}, std::byte{0x90},
};

using FieldCameraLoadFn = void (__thiscall*)(void* fieldCamera);
using SceneCameraDefaultsFn = void* (__thiscall*)(void* cameraState);
using SceneCameraPerspectiveFn = void (__thiscall*)(
    void* sceneCamera, float fov, float nearPlane, float farPlane,
    float aspect, int tracked);
using VfxCameraProjectionUpdateFn = int (__thiscall*)(void* camera);
using GuiMatrixUploadFn = int (__thiscall*)(
    void* shaderState, const float* matrix);
using GuiTaskDrawFn = char (__thiscall*)(void* task);
using GuiScissorSubmitFn = int (__cdecl*)(
    int x, int y, int width, int height);
using MoviePresentFn = void (__thiscall*)(void*, void*, int, int);
using PrimitiveSubmitFn = void (__cdecl*)(
    int, int, int, int, const void*);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_fieldCameraLoadDetour;
patch::Detour32 g_sceneCameraDefaultsDetour;
patch::Detour32 g_sceneCameraPerspectiveDetour;
patch::Detour32 g_vfxCameraProjectionUpdateDetour;
patch::Detour32 g_guiMatrixUploadDetour;
patch::Detour32 g_guiTaskDrawDetour;
patch::Detour32 g_guiScissorSubmitDetour;
patch::Detour32 g_moviePresentDetour;
std::unique_ptr<patch::Transaction> g_moviePrimitiveCallPatch;
std::unique_ptr<patch::Transaction> g_finalCompositePatch;
FieldCameraLoadFn g_originalFieldCameraLoad{};
SceneCameraDefaultsFn g_originalSceneCameraDefaults{};
SceneCameraPerspectiveFn g_originalSceneCameraPerspective{};
VfxCameraProjectionUpdateFn g_originalVfxCameraProjectionUpdate{};
GuiMatrixUploadFn g_originalGuiMatrixUpload{};
GuiTaskDrawFn g_originalGuiTaskDraw{};
GuiScissorSubmitFn g_originalGuiScissorSubmit{};
MoviePresentFn g_originalMoviePresent{};
PrimitiveSubmitFn g_nativePrimitiveSubmit{};
const std::byte* g_imageBase{};
std::atomic_bool g_loggedFieldCorrection{};
std::atomic_bool g_loggedSceneCameraCorrection{};
std::atomic_bool g_loggedVfxCameraCorrection{};
std::atomic_bool g_loggedGuiCorrection{};
std::atomic_bool g_loggedNaviMapBypass{};
std::atomic_bool g_loggedGuiScissorCorrection{};
std::atomic_bool g_loggedMovieFit{};
thread_local unsigned g_guiTaskDrawDepth{};

struct MovieDimensions {
    std::uint32_t width{};
    std::uint32_t height{};
};

struct MovieVertex {
    float x{};
    float y{};
    std::array<std::byte, 12> remaining{};
};

static_assert(sizeof(MovieVertex) == 20u);
thread_local MovieDimensions g_movieDimensions{};

void __fastcall FieldCameraLoadHook(void* fieldCamera, void*) {
    const FieldCameraLoadFn original = g_originalFieldCameraLoad;
    if (!original) return;
    original(fieldCamera);

    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (!fieldCamera || !aspect.valid || !aspect.correctionRequired) return;

    auto* bytes = static_cast<std::byte*>(fieldCamera);
    *reinterpret_cast<float*>(bytes + kFieldAspectOffset) = aspect.value;
    if (!g_loggedFieldCorrection.exchange(true, std::memory_order_acq_rel)) {
        Log("XIII-2 field camera aspect corrected: %ux%u (%.6f)",
            aspect.width, aspect.height, aspect.value);
    }
}

void* __fastcall SceneCameraDefaultsHook(void* cameraState, void*) {
    const SceneCameraDefaultsFn original = g_originalSceneCameraDefaults;
    if (!original) return cameraState;

    void* result = original(cameraState);
    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (cameraState && aspect.valid && aspect.correctionRequired) {
        auto* bytes = static_cast<std::byte*>(cameraState);
        *reinterpret_cast<float*>(bytes + kSceneCameraAspectOffset) =
            aspect.value;
    }
    return result;
}

void __fastcall SceneCameraPerspectiveHook(
    void* sceneCamera, void*, float fov, float nearPlane, float farPlane,
    float aspectValue, int tracked) {
    const SceneCameraPerspectiveFn original = g_originalSceneCameraPerspective;
    if (!original) return;

    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (aspect.valid && aspect.correctionRequired) {
        aspectValue = aspect.value;
        if (!g_loggedSceneCameraCorrection.exchange(
                true, std::memory_order_acq_rel)) {
            LogInfo("XIII-2 SceneCamera aspect corrected: %ux%u (%.6f)",
                    aspect.width, aspect.height, aspect.value);
        }
    }
    original(sceneCamera, fov, nearPlane, farPlane, aspectValue, tracked);
}

int __fastcall VfxCameraProjectionUpdateHook(void* camera, void*) {
    const VfxCameraProjectionUpdateFn original =
        g_originalVfxCameraProjectionUpdate;
    if (!original) return 0;

    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (camera && aspect.valid && aspect.correctionRequired) {
        auto* bytes = static_cast<std::byte*>(camera);
        *reinterpret_cast<float*>(bytes + kVfxCameraAspectOffset) =
            aspect.value;
        if (!g_loggedVfxCameraCorrection.exchange(
                true, std::memory_order_acq_rel)) {
            LogInfo("XIII-2 VFX camera aspect corrected: %ux%u (%.6f)",
                    aspect.width, aspect.height, aspect.value);
        }
    }
    return original(camera);
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
        if (!g_loggedNaviMapBypass.exchange(
                true, std::memory_order_acq_rel)) {
            Log("XIII-2 GUI safe area retained the native GuiNaviMap projection");
        }
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
    if (!g_loggedGuiCorrection.exchange(true, std::memory_order_acq_rel)) {
        Log("XIII-2 GUI safe area active: scale=%.6f,%.6f target=%ux%u",
            aspect.safeScaleX, aspect.safeScaleY,
            aspect.width, aspect.height);
    }
    return original(shaderState, corrected.data());
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
    const display::ScissorRect corrected =
        display::FitScissorRectToSafeArea(
            {x, y, static_cast<std::int32_t>(right),
             static_cast<std::int32_t>(bottom)},
            aspect);

    if (!g_loggedGuiScissorCorrection.exchange(
            true, std::memory_order_acq_rel)) {
        Log("XIII-2 GUI scissors mapped to the centred 16:9 safe area");
    }
    return original(
        corrected.left, corrected.top,
        corrected.right - corrected.left,
        corrected.bottom - corrected.top);
}

void __fastcall MoviePresentHook(
    void* manager, void*, void* destination, int a3, int a4) {
    const MoviePresentFn original = g_originalMoviePresent;
    if (!original) return;

    const MovieDimensions previous = g_movieDimensions;
    if (manager) {
        const auto* bytes = static_cast<const std::byte*>(manager);
        g_movieDimensions.width = *reinterpret_cast<const std::uint32_t*>(
            bytes + kMovieWidthOffset);
        g_movieDimensions.height = *reinterpret_cast<const std::uint32_t*>(
            bytes + kMovieHeightOffset);
    }
    original(manager, destination, a3, a4);
    g_movieDimensions = previous;
}

void __cdecl MoviePrimitiveSubmitHook(
    int primitiveType, int enabled, int format, int stride,
    const void* vertices) {
    const PrimitiveSubmitFn native = g_nativePrimitiveSubmit;
    if (!native) return;

    const display::AspectRatio target = display::CurrentAspectRatio();
    const display::AspectFitScale fit = display::ResolveAspectFitScale(
        g_movieDimensions.width, g_movieDimensions.height,
        target.width, target.height);
    if (!vertices || primitiveType != 6 || enabled != 4 || stride != 20 ||
        !fit.valid || !fit.correctionRequired) {
        native(primitiveType, enabled, format, stride, vertices);
        return;
    }

    std::array<MovieVertex, 4> corrected{};
    std::memcpy(corrected.data(), vertices, sizeof(corrected));
    for (MovieVertex& vertex : corrected) {
        vertex.x *= fit.x;
        vertex.y *= fit.y;
    }
    if (!g_loggedMovieFit.exchange(true, std::memory_order_acq_rel)) {
        Log("XIII-2 prerender aspect fit active: movie=%ux%u target=%ux%u scale=%.6f,%.6f",
            g_movieDimensions.width, g_movieDimensions.height,
            target.width, target.height, fit.x, fit.y);
    }
    native(primitiveType, enabled, format, stride, corrected.data());
}

BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::ArbitraryAspectRatio)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII-2 aspect ratio correction disabled: main image is unavailable");
        return TRUE;
    }

    const auto fieldCameraLoad = image->At(
        kFieldCameraLoadRva, kFieldCameraLoadPrefix.size());
    const auto sceneCameraDefaults = image->At(
        kSceneCameraDefaultsRva, kSceneCameraDefaultsPrefix.size());
    const auto sceneCameraPerspective = image->At(
        kSceneCameraPerspectiveRva, kSceneCameraPerspectivePrefix.size());
    const auto vfxCameraProjectionUpdate = image->At(
        kVfxCameraProjectionUpdateRva,
        kVfxCameraProjectionUpdatePrefix.size());
    const auto guiMatrixUpload = image->At(
        kGuiMatrixUploadRva, kGuiMatrixUploadPrefix.size());
    const auto guiTaskDraw = image->At(
        kGuiTaskDrawRva, kGuiTaskDrawPrefix.size());
    const auto guiScissorSubmit = image->At(
        kGuiScissorSubmitRva, kGuiScissorSubmitPrefix.size());
    const auto moviePresent = image->At(
        kMoviePresentRva, kMoviePresentPrefix.size());
    const auto moviePrimitiveCall = image->At(
        kMoviePrimitiveCallRva, kMoviePrimitiveCall.size());
    const auto nativePrimitiveSubmit = image->At(
        kNativePrimitiveSubmitRva, 1u);
    const auto finalCompositeMode = image->At(
        kFinalCompositeModeRva, kFinalCompositeModeLoad.size());
    if (!fieldCameraLoad || !sceneCameraDefaults ||
        !sceneCameraPerspective || !vfxCameraProjectionUpdate ||
        !guiMatrixUpload || !guiTaskDraw || !guiScissorSubmit ||
        !moviePresent ||
        !moviePrimitiveCall || !nativePrimitiveSubmit ||
        !finalCompositeMode ||
        std::memcmp(fieldCameraLoad->data(), kFieldCameraLoadPrefix.data(),
                    kFieldCameraLoadPrefix.size()) != 0 ||
        std::memcmp(sceneCameraDefaults->data(),
                    kSceneCameraDefaultsPrefix.data(),
                    kSceneCameraDefaultsPrefix.size()) != 0 ||
        std::memcmp(sceneCameraPerspective->data(),
                    kSceneCameraPerspectivePrefix.data(),
                    kSceneCameraPerspectivePrefix.size()) != 0 ||
        std::memcmp(vfxCameraProjectionUpdate->data(),
                    kVfxCameraProjectionUpdatePrefix.data(),
                    kVfxCameraProjectionUpdatePrefix.size()) != 0 ||
        std::memcmp(guiMatrixUpload->data(), kGuiMatrixUploadPrefix.data(),
                    kGuiMatrixUploadPrefix.size()) != 0 ||
        std::memcmp(guiTaskDraw->data(), kGuiTaskDrawPrefix.data(),
                    kGuiTaskDrawPrefix.size()) != 0 ||
        std::memcmp(guiScissorSubmit->data(),
                    kGuiScissorSubmitPrefix.data(),
                    kGuiScissorSubmitPrefix.size()) != 0 ||
        std::memcmp(moviePresent->data(), kMoviePresentPrefix.data(),
                    kMoviePresentPrefix.size()) != 0 ||
        std::memcmp(moviePrimitiveCall->data(), kMoviePrimitiveCall.data(),
                    kMoviePrimitiveCall.size()) != 0 ||
        std::memcmp(finalCompositeMode->data(),
                    kFinalCompositeModeLoad.data(),
                    kFinalCompositeModeLoad.size()) != 0) {
        LogWarning("XIII-2 aspect ratio correction disabled: executable contract mismatch");
        return TRUE;
    }

    g_imageBase = image->Bytes().data();
    g_nativePrimitiveSubmit = reinterpret_cast<PrimitiveSubmitFn>(
        const_cast<std::byte*>(nativePrimitiveSubmit->data()));
    const auto revertHooks = [] {
        g_moviePresentDetour.RevertAndClear(g_originalMoviePresent);
        g_guiScissorSubmitDetour.RevertAndClear(
            g_originalGuiScissorSubmit);
        g_guiTaskDrawDetour.RevertAndClear(g_originalGuiTaskDraw);
        g_guiMatrixUploadDetour.RevertAndClear(g_originalGuiMatrixUpload);
        g_vfxCameraProjectionUpdateDetour.RevertAndClear(
            g_originalVfxCameraProjectionUpdate);
        g_sceneCameraPerspectiveDetour.RevertAndClear(
            g_originalSceneCameraPerspective);
        g_sceneCameraDefaultsDetour.RevertAndClear(
            g_originalSceneCameraDefaults);
        g_fieldCameraLoadDetour.RevertAndClear(g_originalFieldCameraLoad);
        g_nativePrimitiveSubmit = nullptr;
        g_imageBase = nullptr;
    };

    if (!g_fieldCameraLoadDetour.Install(
            "ff13-2-field-camera-aspect",
            const_cast<std::byte*>(fieldCameraLoad->data()),
            reinterpret_cast<void*>(&FieldCameraLoadHook),
            kFieldCameraLoadPrefix,
            reinterpret_cast<void**>(&g_originalFieldCameraLoad)) ||
        !g_sceneCameraDefaultsDetour.Install(
            "ff13-2-scene-camera-default-aspect",
            const_cast<std::byte*>(sceneCameraDefaults->data()),
            reinterpret_cast<void*>(&SceneCameraDefaultsHook),
            kSceneCameraDefaultsPrefix,
            reinterpret_cast<void**>(&g_originalSceneCameraDefaults)) ||
        !g_sceneCameraPerspectiveDetour.Install(
            "ff13-2-scene-camera-perspective-aspect",
            const_cast<std::byte*>(sceneCameraPerspective->data()),
            reinterpret_cast<void*>(&SceneCameraPerspectiveHook),
            kSceneCameraPerspectivePrefix,
            reinterpret_cast<void**>(&g_originalSceneCameraPerspective)) ||
        !g_vfxCameraProjectionUpdateDetour.Install(
            "ff13-2-vfx-camera-aspect",
            const_cast<std::byte*>(vfxCameraProjectionUpdate->data()),
            reinterpret_cast<void*>(&VfxCameraProjectionUpdateHook),
            kVfxCameraProjectionUpdatePrefix,
            reinterpret_cast<void**>(&g_originalVfxCameraProjectionUpdate)) ||
        !g_guiMatrixUploadDetour.Install(
            "ff13-2-gui-matrix-safe-area",
            const_cast<std::byte*>(guiMatrixUpload->data()),
            reinterpret_cast<void*>(&GuiMatrixUploadHook),
            kGuiMatrixUploadPrefix,
            reinterpret_cast<void**>(&g_originalGuiMatrixUpload)) ||
        !g_guiTaskDrawDetour.Install(
            "ff13-2-gui-task-aspect-scope",
            const_cast<std::byte*>(guiTaskDraw->data()),
            reinterpret_cast<void*>(&GuiTaskDrawHook),
            kGuiTaskDrawPrefix,
            reinterpret_cast<void**>(&g_originalGuiTaskDraw)) ||
        !g_guiScissorSubmitDetour.Install(
            "ff13-2-gui-scissor-safe-area",
            const_cast<std::byte*>(guiScissorSubmit->data()),
            reinterpret_cast<void*>(&GuiScissorSubmitHook),
            kGuiScissorSubmitPrefix,
            reinterpret_cast<void**>(&g_originalGuiScissorSubmit)) ||
        !g_moviePresentDetour.Install(
            "ff13-2-prerender-aspect-scope",
            const_cast<std::byte*>(moviePresent->data()),
            reinterpret_cast<void*>(&MoviePresentHook),
            kMoviePresentPrefix,
            reinterpret_cast<void**>(&g_originalMoviePresent))) {
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: detour installation failed");
        return TRUE;
    }

    const auto movieReplacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(moviePrimitiveCall->data()),
        reinterpret_cast<std::uintptr_t>(&MoviePrimitiveSubmitHook));
    auto moviePatch = std::make_unique<patch::Transaction>(
        "ff13-2-prerender-aspect-fit",
        std::span<std::byte>(
            const_cast<std::byte*>(moviePrimitiveCall->data()),
            moviePrimitiveCall->size()),
        patch::registry::Kind::Detour);
    if (!movieReplacement || !moviePatch->Add(
            0u, kMoviePrimitiveCall, *movieReplacement)) {
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: invalid prerender patch");
        return TRUE;
    }
    const patch::ApplyStatus movieStatus = moviePatch->Apply();
    if (movieStatus != patch::ApplyStatus::Applied &&
        movieStatus != patch::ApplyStatus::AlreadyApplied) {
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: prerender patch status=%s",
                   patch::ApplyStatusName(movieStatus));
        return TRUE;
    }

    auto compositePatch = std::make_unique<patch::Transaction>(
        "ff13-2-final-composite-aspect",
        std::span<std::byte>(
            const_cast<std::byte*>(finalCompositeMode->data()),
            finalCompositeMode->size()));
    if (!compositePatch->Add(
            0u, kFinalCompositeModeLoad, kFullScreenCompositeMode)) {
        moviePatch->Revert();
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: invalid final composite patch");
        return TRUE;
    }
    const patch::ApplyStatus compositeStatus = compositePatch->Apply();
    if (compositeStatus != patch::ApplyStatus::Applied &&
        compositeStatus != patch::ApplyStatus::AlreadyApplied) {
        moviePatch->Revert();
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: final composite patch status=%s",
                   patch::ApplyStatusName(compositeStatus));
        return TRUE;
    }

    g_moviePrimitiveCallPatch = std::move(moviePatch);
    g_finalCompositePatch = std::move(compositePatch);
    Log("XIII-2 arbitrary aspect ratio support installed for scene/VFX cameras, GUI, prerenders, and final presentation");
    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &InitializeOnce, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_aspect_ratio
