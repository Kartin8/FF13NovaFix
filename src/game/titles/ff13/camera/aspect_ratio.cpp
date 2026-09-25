#include "game/titles/ff13/camera/aspect_ratio.h"
#include "game/titles/ff13/camera/aspect_ratio_internal.h"

#include "diagnostics/log.h"
#include "display/aspect_runtime.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/ff13/rendering/composition_aspect.h"
#include "game/titles/ff13/rendering/movie_aspect.h"
#include "game/titles/ff13/ui/backdrop/backdrop_capture.h"
#include "game/titles/ff13/ui/gui_aspect.h"
#include "game/titles/ff13/ui/map_alert_vfx.h"
#include "game/titles/ff13/ui/menu/menu_aspect.h"
#include "patch/detour32.h"
#include "patch/transaction.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace novafix::game::ff13_aspect_ratio {
namespace {

constexpr std::uint32_t kPerspectiveRva = 0x7BC690u - 0x400000u;
constexpr std::uint32_t kCameraDefaultsRva = 0x7BEBC0u - 0x400000u;
constexpr std::uint32_t kVfxProjectionRva = 0xCE3C70u - 0x400000u;

constexpr std::array<std::byte, 9> kPerspectivePrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x18}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xE8}};
constexpr std::array<std::byte, 9> kCameraDefaultsPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x20}, std::byte{0x89}, std::byte{0x4D},
    std::byte{0xE0}};
constexpr std::array<std::byte, 6> kVfxProjectionPrefix{
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x40}, std::byte{0x56},
    std::byte{0x8B}, std::byte{0xF1}};

struct ResolutionBranch {
    std::uint32_t rva;
    std::array<std::byte, 2> expected;
};

constexpr std::array<ResolutionBranch, 12> kResolutionBranches{{
    {0xEC1E9Du - 0x400000u, {std::byte{0x74}, std::byte{0x2A}}},
    {0xEC1EABu - 0x400000u, {std::byte{0x74}, std::byte{0x1C}}},
    {0xEC1F4Cu - 0x400000u, {std::byte{0x74}, std::byte{0x34}}},
    {0xEC1F58u - 0x400000u, {std::byte{0x74}, std::byte{0x28}}},
    {0xEC2348u - 0x400000u, {std::byte{0x74}, std::byte{0x24}}},
    {0xEC2357u - 0x400000u, {std::byte{0x74}, std::byte{0x14}}},
    {0xEC4C6Cu - 0x400000u, {std::byte{0x74}, std::byte{0x2A}}},
    {0xEC4C78u - 0x400000u, {std::byte{0x74}, std::byte{0x1E}}},
    {0xEC4D4Cu - 0x400000u, {std::byte{0x74}, std::byte{0x2A}}},
    {0xEC4D58u - 0x400000u, {std::byte{0x74}, std::byte{0x1E}}},
    {0xEC528Cu - 0x400000u, {std::byte{0x74}, std::byte{0x2A}}},
    {0xEC5298u - 0x400000u, {std::byte{0x74}, std::byte{0x1E}}},
}};
constexpr std::array<std::byte, 2> kBranchNops{
    std::byte{0x90}, std::byte{0x90}};
constexpr std::array<std::byte, 2> kResolutionPredicateBranch{
    std::byte{0x74}, std::byte{0x0F}};
constexpr std::array<std::byte, 2> kResolutionPredicateOr{
    std::byte{0x75}, std::byte{0xEC1E88u - (0xEC1E7Au + 2u)}};
using PerspectiveFn = int (__thiscall*)(void*, float, float, float, float, int);
using CameraDefaultsFn = void* (__thiscall*)(void*);
using VfxProjectionFn = int (__thiscall*)(void*);

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_active{};
patch::Detour32 g_perspectiveDetour;
patch::Detour32 g_cameraDefaultsDetour;
patch::Detour32 g_vfxProjectionDetour;
std::unique_ptr<patch::Transaction> g_calls;
PerspectiveFn g_perspective{};
CameraDefaultsFn g_cameraDefaults{};
VfxProjectionFn g_vfxProjection{};
template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

int __fastcall PerspectiveHook(void* camera, void*, float fov,
                              float nearPlane, float farPlane,
                              float aspectValue, int tracked) {
    const auto aspect = display::CurrentAspectRatio();
    if (aspect.valid && aspect.correctionRequired) aspectValue = aspect.value;
    return g_perspective(camera, fov, nearPlane, farPlane, aspectValue, tracked);
}

void* __fastcall CameraDefaultsHook(void* state, void*) {
    void* result = g_cameraDefaults(state);
    const auto aspect = display::CurrentAspectRatio();
    if (state && aspect.valid && aspect.correctionRequired) {
        Field<float>(state, 0x1Cu) = aspect.value;
    }
    return result;
}

int __fastcall VfxProjectionHook(void* camera, void*) {
    const auto aspect = display::CurrentAspectRatio();
    if (!camera || !aspect.valid || !aspect.correctionRequired ||
        (Field<std::uint16_t>(camera, 0x14Eu) & 0x2000u) != 0u) {
        return g_vfxProjection(camera);
    }
    const float originalAspect = Field<float>(camera, 0x134u);
    Field<float>(camera, 0x134u) = aspect.value;
    Field<std::uint16_t>(camera, 0x14Eu) |= 1u;
    const int result = g_vfxProjection(camera);
    Field<float>(camera, 0x134u) = originalAspect;
    return result;
}

BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII ||
        !Supports(Capability::ArbitraryAspectRatio)) return TRUE;
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) return TRUE;

    const auto target = [&image](std::uint32_t rva) -> void* {
        const auto range = image->At(rva, 1u);
        return range ? const_cast<std::byte*>(range->data()) : nullptr;
    };
    const auto revert = [] {
        ff13_movie_aspect::Revert();
        ff13_ui::menu_aspect::Revert();
        ff13_ui::gui_aspect::Revert();
        g_vfxProjectionDetour.RevertAndClear(g_vfxProjection);
        g_cameraDefaultsDetour.RevertAndClear(g_cameraDefaults);
        g_perspectiveDetour.RevertAndClear(g_perspective);
    };

    auto calls = std::make_unique<patch::Transaction>(
        "ff13-aspect-presentation", std::span<std::byte>(
            const_cast<std::byte*>(image->Bytes().data()), image->Bytes().size()),
        patch::registry::Kind::Detour);
    if (!ff13_ui::gui_aspect::RegisterMinimapAspectCalls(*calls, *image)) {
        LogWarning("XIII aspect support disabled: invalid minimap camera contract");
        return TRUE;
    }
    if (!ff13_ui::gui_aspect::RegisterMinimapCameraCalls(*calls, *image)) {
        LogWarning("XIII aspect support disabled: invalid minimap matrix contract");
        return TRUE;
    }
    for (const auto& branch : kResolutionBranches) {
        if (!calls->Add(branch.rva, branch.expected, kBranchNops)) {
            LogWarning("XIII aspect support disabled: invalid resolution contract");
            return TRUE;
        }
    }
    if (!calls->Add(0xEC1E7Au - 0x400000u,
                    kResolutionPredicateBranch, kResolutionPredicateOr)) {
        LogWarning("XIII aspect support disabled: invalid resolution predicate");
        return TRUE;
    }
    if (!ff13_ui::menu_aspect::RegisterProductUpdatePatch(*calls, *image)) {
        LogWarning("XIII aspect support disabled: invalid menu update dispatch");
        return TRUE;
    }
    if (!ff13_ui::backdrop_capture::RegisterAllocationCalls(*calls, *image)) {
        LogWarning("XIII aspect support disabled: invalid backdrop allocation contract");
        return TRUE;
    }
    if (!ff13_ui::backdrop_capture::RegisterFlushCalls(*calls, *image)) {
        LogWarning("XIII aspect support disabled: invalid backdrop submission contract");
        return TRUE;
    }
    if (!ff13_composition_aspect::RegisterCompositeCalls(*calls, *image) ||
        !ff13_movie_aspect::RegisterPrimitiveCall(*calls, *image) ||
        !ff13_ui::gui_aspect::RegisterProjectionCalls(*calls, *image) ||
        !ff13_ui::backdrop_capture::RegisterCaptureCalls(*calls, *image) ||
        !ff13_ui::menu_aspect::RegisterNodeTransformCall(*calls, *image) ||
        !ff13_ui::menu_aspect::RegisterListScissorCalls(*calls, *image) ||
        !ff13_composition_aspect::RegisterSnapshotCall(*calls, *image)) {
        LogWarning("XIII aspect support disabled: invalid presentation contract");
        return TRUE;
    }
    ff13_composition_aspect::Prepare(*image);
    ff13_movie_aspect::Prepare(*image);
    ff13_ui::gui_aspect::Prepare(*image);
    ff13_ui::backdrop_capture::Prepare(*image);
    ff13_ui::menu_aspect::Prepare(*image);

    if (!g_perspectiveDetour.Install(
            "ff13-camera-aspect", target(kPerspectiveRva),
            reinterpret_cast<void*>(&PerspectiveHook), kPerspectivePrefix,
            reinterpret_cast<void**>(&g_perspective)) ||
        !g_cameraDefaultsDetour.Install(
            "ff13-camera-default-aspect", target(kCameraDefaultsRva),
            reinterpret_cast<void*>(&CameraDefaultsHook), kCameraDefaultsPrefix,
            reinterpret_cast<void**>(&g_cameraDefaults)) ||
        !g_vfxProjectionDetour.Install(
            "ff13-vfx-camera-aspect", target(kVfxProjectionRva),
            reinterpret_cast<void*>(&VfxProjectionHook), kVfxProjectionPrefix,
            reinterpret_cast<void**>(&g_vfxProjection)) ||
        !ff13_ui::gui_aspect::InstallDetours(*image) ||
        !ff13_ui::menu_aspect::InstallDetours(*image) ||
        !ff13_movie_aspect::InstallPresentDetour(*image)) {
        revert();
        LogWarning("XIII aspect support disabled: camera/UI contract mismatch");
        return TRUE;
    }
    const auto status = calls->Apply();
    if (status != patch::ApplyStatus::Applied &&
        status != patch::ApplyStatus::AlreadyApplied) {
        revert();
        LogWarning("XIII aspect support disabled: presentation patch status=%s",
                   patch::ApplyStatusName(status));
        return TRUE;
    }
    g_calls = std::move(calls);
    g_active.store(true, std::memory_order_release);
    ff13_ui::map_alert_vfx::Install(*image);
    return TRUE;
}

}

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &InitializeOnce, nullptr, nullptr);
}

bool IsActive() {
    return g_active.load(std::memory_order_acquire);
}

bool IsDrawingGui() {
    return g_guiDrawDepth != 0u;
}

}
