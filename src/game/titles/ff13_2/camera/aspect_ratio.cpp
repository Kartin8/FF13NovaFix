#include "game/titles/ff13_2/camera/aspect_ratio.h"

#include "diagnostics/log.h"
#include "display/aspect_runtime.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/ff13_2/ui/gui_aspect.h"
#include "game/titles/ff13_2/ui/map_alert_vfx.h"
#include "game/titles/ff13_2/ui/title_vfx_aspect.h"
#include "game/titles/ff13_2/rendering/movie_aspect.h"
#include "game/titles/ff13_2/rendering/internal_resolution.h"
#include "patch/detour32.h"
#include "patch/transaction.h"

#include <windows.h>

#include <array>
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
constexpr std::size_t kFieldAspectOffset = 0x0A1Cu;
constexpr std::size_t kSceneCameraAspectOffset = 0x30u;
constexpr std::size_t kVfxCameraAspectOffset = 0x138u;

constexpr std::array<std::byte, 13> kFieldCameraLoadPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xE4}, std::byte{0xF0}, std::byte{0x83}, std::byte{0xEC},
    std::byte{0x14}, std::byte{0x53}, std::byte{0x56}, std::byte{0x8B},
    std::byte{0xF1},
};
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
using FieldCameraLoadFn = void (__thiscall*)(void* fieldCamera);
using SceneCameraDefaultsFn = void* (__thiscall*)(void* cameraState);
using SceneCameraPerspectiveFn = void (__thiscall*)(
    void* sceneCamera, float fov, float nearPlane, float farPlane,
    float aspect, int tracked);
using VfxCameraProjectionUpdateFn = int (__thiscall*)(void* camera);
INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_fieldCameraLoadDetour;
patch::Detour32 g_sceneCameraDefaultsDetour;
patch::Detour32 g_sceneCameraPerspectiveDetour;
patch::Detour32 g_vfxCameraProjectionUpdateDetour;
std::unique_ptr<patch::Transaction> g_minimapCallsPatch;
FieldCameraLoadFn g_originalFieldCameraLoad{};
SceneCameraDefaultsFn g_originalSceneCameraDefaults{};
SceneCameraPerspectiveFn g_originalSceneCameraPerspective{};
VfxCameraProjectionUpdateFn g_originalVfxCameraProjectionUpdate{};

void __fastcall FieldCameraLoadHook(void* fieldCamera, void*) {
    const FieldCameraLoadFn original = g_originalFieldCameraLoad;
    if (!original) return;
    original(fieldCamera);

    const display::AspectRatio aspect = display::CurrentAspectRatio();
    if (!fieldCamera || !aspect.valid || !aspect.correctionRequired) return;

    auto* bytes = static_cast<std::byte*>(fieldCamera);
    *reinterpret_cast<float*>(bytes + kFieldAspectOffset) = aspect.value;
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
    }
    return original(camera);
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
    if (!fieldCameraLoad || !sceneCameraDefaults ||
        !sceneCameraPerspective || !vfxCameraProjectionUpdate ||
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
        !xiii2_ui::gui_aspect::MatchesContract(*image)) {
        LogWarning("XIII-2 aspect ratio correction disabled: executable contract mismatch");
        return TRUE;
    }
    if (!xiii2_movie_aspect::Prepare(*image)) {
        LogWarning("XIII-2 aspect ratio correction disabled: movie contract mismatch");
        return TRUE;
    }

    xiii2_ui::gui_aspect::Prepare(*image);
    auto minimapCallsPatch = std::make_unique<patch::Transaction>(
        "ff13-2-minimap-camera",
        std::span<std::byte>(const_cast<std::byte*>(image->Bytes().data()),
                             image->Bytes().size()),
        patch::registry::Kind::Detour);
    if (!xiii2_ui::gui_aspect::RegisterMinimapAspectCalls(
            *minimapCallsPatch, *image)) {
        LogWarning("XIII-2 aspect ratio correction disabled: invalid minimap aspect contract");
        xiii2_ui::gui_aspect::ResetPreparedState();
        return TRUE;
    }
    if (!xiii2_ui::gui_aspect::RegisterMinimapMatrixCall(
            *minimapCallsPatch, *image)) {
        LogWarning("XIII-2 aspect ratio correction disabled: invalid minimap matrix contract");
        xiii2_ui::gui_aspect::ResetPreparedState();
        return TRUE;
    }
    if (!xiii2_internal_resolution::RegisterPatches(
            *minimapCallsPatch, *image)) {
        LogWarning("XIII-2 aspect ratio correction disabled: invalid internal resolution contract");
        xiii2_ui::gui_aspect::ResetPreparedState();
        return TRUE;
    }
    xiii2_ui::gui_aspect::PrepareNativeAspect(*image);
    const auto revertHooks = [] {
        xiii2_ui::gui_aspect::RevertMinimapDraw();
        xiii2_movie_aspect::Revert();
        xiii2_ui::gui_aspect::RevertGui();
        xiii2_ui::title_vfx_aspect::Revert();
        g_vfxCameraProjectionUpdateDetour.RevertAndClear(
            g_originalVfxCameraProjectionUpdate);
        g_sceneCameraPerspectiveDetour.RevertAndClear(
            g_originalSceneCameraPerspective);
        g_sceneCameraDefaultsDetour.RevertAndClear(
            g_originalSceneCameraDefaults);
        g_fieldCameraLoadDetour.RevertAndClear(g_originalFieldCameraLoad);
        xiii2_ui::gui_aspect::ResetPreparedState();
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
        !xiii2_ui::gui_aspect::InstallDetours(*image) ||
        !xiii2_ui::title_vfx_aspect::Install(*image) ||
        !xiii2_movie_aspect::InstallPresentDetour(*image)) {
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: detour installation failed");
        return TRUE;
    }

    const auto minimapStatus = minimapCallsPatch->Apply();
    if (minimapStatus != patch::ApplyStatus::Applied &&
        minimapStatus != patch::ApplyStatus::AlreadyApplied) {
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: minimap camera patch status=%s",
                   patch::ApplyStatusName(minimapStatus));
        return TRUE;
    }

    if (!xiii2_movie_aspect::ApplyPrimitivePatch(*image)) {
        minimapCallsPatch->Revert();
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: prerender patch failed");
        return TRUE;
    }

    if (!xiii2_movie_aspect::ApplyCompositePatch(*image)) {
        minimapCallsPatch->Revert();
        revertHooks();
        LogWarning("XIII-2 aspect ratio correction disabled: final composite patch failed");
        return TRUE;
    }

    g_minimapCallsPatch = std::move(minimapCallsPatch);
    xiii2_ui::map_alert_vfx::Install(*image);
    return TRUE;
}

} // namespace

MinimapClipCorrection CurrentMinimapClipCorrection() {
    return xiii2_ui::gui_aspect::CurrentMinimapClipCorrection();
}

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &InitializeOnce, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_aspect_ratio
