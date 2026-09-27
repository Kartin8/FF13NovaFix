#include "d3d9/device/device_hook_installer.h"

#include "compat/compatibility.h"
#include "diagnostics/log.h"
#include "d3d9/device/device_shader_hooks.h"
#include "d3d9/device/device_state_hooks.h"
#include "d3d9/device/mip_lod_bias_controller.h"
#include "d3d9/device/redundant_state_filter.h"
#include "game/core/game_profile.h"
#include "patch/vtable_hook.h"
#include "settings/graphics_settings.h"
#include "settings/performance_settings.h"

namespace novafix::d3d9::device_hook_installer {
namespace {

using namespace device_registry;

void ReactivateDeviceHooks(
    IDirect3DDevice9* device, void** table, DeviceVtableRecord* vtableRecord) {
    if (vtableRecord->vtable) {
        bool reactivated = true;
        for (const patch::registry::ClaimId claim : vtableRecord->claims) {
            if (claim &&
                !patch::registry::ReactivateVtableClaim(claim)) {
                reactivated = false;
            }
        }
        if (!reactivated) {
            LogError("IDirect3DDevice9 hook reactivation failed: device=%p table=%p",
                device, table);
        }
    }
    device_shader_hooks::Reactivate(table);
    device_state_hooks::Reactivate(table);
}

void InstallDeviceHooks(
    void** table, DeviceVtableRecord* vtableRecord,
    const DeviceHookPolicy& hookPolicy, const EntryPoints& entryPoints) {
    device_state_hooks::Install(table, hookPolicy);
    device_shader_hooks::Install(table, hookPolicy);
    if (!vtableRecord->vtable) {
        vtableRecord->vtable = table;
        void* originalRelease = nullptr;
        void* originalCreateAdditional = nullptr;
        void* originalGetSwapChain = nullptr;
        void* originalReset = nullptr;
        void* originalPresent = nullptr;
        void* originalSetScissorRect = nullptr;
        void* originalDrawPrimitiveUP = nullptr;
        void* originalVertexBuffer = nullptr;
        const auto replaceOptional =
            [table](bool enabled, const char* feature, size_t index,
                    void* replacement, void** original,
                    patch::registry::ClaimId* claim) {
                return enabled && ReplaceVtableSlot(
                    feature, table, index, replacement, original, claim);
            };
        const bool releaseOk = ReplaceVtableSlot(
            "d3d9-device:release", table, 2,
            reinterpret_cast<void*>(entryPoints.release), &originalRelease,
            Claim(*vtableRecord, DeviceClaim::Release));
        const bool createAdditionalOk = ReplaceVtableSlot(
            "d3d9-device:create-additional-swap-chain", table, 13,
            reinterpret_cast<void*>(entryPoints.createAdditionalSwapChain), &originalCreateAdditional,
            Claim(*vtableRecord, DeviceClaim::CreateAdditionalSwapChain));
        const bool getSwapChainOk = ReplaceVtableSlot(
            "d3d9-device:get-swap-chain", table, 14,
            reinterpret_cast<void*>(entryPoints.getSwapChain), &originalGetSwapChain,
            Claim(*vtableRecord, DeviceClaim::GetSwapChain));
        const bool resetOk = ReplaceVtableSlot(
            "d3d9-device:reset", table, 16,
            reinterpret_cast<void*>(entryPoints.reset),
            &originalReset,
            Claim(*vtableRecord, DeviceClaim::Reset));
        const bool presentOk = ReplaceVtableSlot(
            "d3d9-device:present", table, 17,
            reinterpret_cast<void*>(entryPoints.present), &originalPresent,
            Claim(*vtableRecord, DeviceClaim::Present));
        const bool scissorOk = replaceOptional(
            hookPolicy.setScissorRect, "d3d9-device:set-scissor-rect", 75,
            reinterpret_cast<void*>(entryPoints.setScissorRect), &originalSetScissorRect,
            Claim(*vtableRecord, DeviceClaim::SetScissorRect));
        const bool vertexBufferOk = replaceOptional(
            hookPolicy.createVertexBuffer, "d3d9-device:create-vertex-buffer", 26,
            reinterpret_cast<void*>(entryPoints.createVertexBuffer),
            &originalVertexBuffer,
            Claim(*vtableRecord, DeviceClaim::CreateVertexBuffer));
        const bool drawPrimitiveUpOk = replaceOptional(
            hookPolicy.drawPrimitiveUp, "d3d9-device:draw-primitive-up", 83,
            reinterpret_cast<void*>(entryPoints.drawPrimitiveUP),
            &originalDrawPrimitiveUP,
            Claim(*vtableRecord, DeviceClaim::DrawPrimitiveUp));
        if (releaseOk) vtableRecord->release = reinterpret_cast<DeviceReleaseFn>(originalRelease);
        if (createAdditionalOk) {
            vtableRecord->createAdditionalSwapChain =
                reinterpret_cast<CreateAdditionalSwapChainFn>(originalCreateAdditional);
        }
        if (getSwapChainOk) vtableRecord->getSwapChain = reinterpret_cast<GetSwapChainFn>(originalGetSwapChain);
        if (resetOk) vtableRecord->reset = reinterpret_cast<DeviceResetFn>(originalReset);
        if (presentOk) vtableRecord->present = reinterpret_cast<DevicePresentFn>(originalPresent);
        if (scissorOk) {
            vtableRecord->setScissorRect = reinterpret_cast<SetScissorRectFn>(originalSetScissorRect);
        }
        if (vertexBufferOk) vtableRecord->createVertexBuffer = reinterpret_cast<CreateVertexBufferFn>(originalVertexBuffer);
        if (drawPrimitiveUpOk) vtableRecord->drawPrimitiveUP = reinterpret_cast<DrawPrimitiveUPFn>(originalDrawPrimitiveUP);
    }
}

void InstallExtendedDeviceHooks(
    void** table, DeviceVtableRecord* vtableRecord,
    const EntryPoints& entryPoints) {
    bool presentExOk = vtableRecord->presentEx != nullptr;
    bool resetExOk = vtableRecord->resetEx != nullptr;
    if (!presentExOk) {
        void* originalPresentEx = nullptr;
        presentExOk = ReplaceVtableSlot(
            "d3d9-device:present-ex", table, 121,
            reinterpret_cast<void*>(entryPoints.presentEx),
            &originalPresentEx,
            Claim(*vtableRecord, DeviceClaim::PresentEx));
        if (presentExOk) {
            vtableRecord->presentEx =
                reinterpret_cast<DevicePresentExFn>(originalPresentEx);
        }
    }
    if (!resetExOk) {
        void* originalResetEx = nullptr;
        resetExOk = ReplaceVtableSlot(
            "d3d9-device:reset-ex", table, 132,
            reinterpret_cast<void*>(entryPoints.resetEx),
            &originalResetEx,
            Claim(*vtableRecord, DeviceClaim::ResetEx));
        if (resetExOk) {
            vtableRecord->resetEx =
                reinterpret_cast<DeviceResetExFn>(originalResetEx);
        }
    }
}

} // namespace

DeviceHookPolicy ConfigureDeviceHookPolicy() {
    const bool compatibilityAllowsChanges =
        compat::compatibility::AllowsNovaFixChanges();
    const settings::GraphicsSettings graphics = settings::Active();
    const settings::PerformanceSettings performance =
        settings::ActivePerformance();
    DeviceHookPolicy hookPolicy = ResolveDeviceHookPolicy({
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::TextureFiltering),
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::UiVertexBufferOptimization),
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::ScreenSpaceAlignment),
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::HighResolutionScissor),
        compatibilityAllowsChanges &&
            game::Supports(game::Capability::ShaderCorrection),
        graphics.optimizeUiVertexBuffer,
        graphics.xiii2ShaderCorrections,
        game::CurrentProfile().title == game::Title::FinalFantasyXIII2,
        compatibilityAllowsChanges &&
            game::CurrentProfile().title == game::Title::FinalFantasyXIII2,
        performance.xiii2OptimizeStateSubmission,
        compatibilityAllowsChanges &&
            game::CurrentProfile().title == game::Title::FinalFantasyXIII2,
    });
    redundant_state_filter::Configure(hookPolicy.redundantStateFilter);
    mip_lod_bias::Configure(
        hookPolicy.setSamplerState,
        game::CurrentProfile().title == game::Title::FinalFantasyXIII2,
        graphics.anisotropicFiltering,
        graphics.xiii2MipLodBias);
    return hookPolicy;
}

void Install(
    IDirect3DDevice9* device, void** table,
    device_registry::DeviceVtableRecord* record,
    const DeviceHookPolicy& hookPolicy, bool extended,
    const EntryPoints& entryPoints) {
    ReactivateDeviceHooks(device, table, record);
    InstallDeviceHooks(table, record, hookPolicy, entryPoints);
    if (extended) InstallExtendedDeviceHooks(table, record, entryPoints);
}

} // namespace novafix::d3d9::device_hook_installer
