#include "d3d9/device/device_hooks.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "compat/compatibility.h"
#include "compat/platform.h"
#include "d3d9/backend.h"
#include "d3d9/device/device_hook_installer.h"
#include "d3d9/device/device_recovery_policy.h"
#include "d3d9/device/device_registry.h"
#include "d3d9/device/device_ui_hooks.h"
#include "d3d9/device/mip_lod_bias_controller.h"
#include "d3d9/presentation/presentation_router.h"
#include "d3d9/presentation/presentation_setup.h"
#include "d3d9/device/redundant_state_filter.h"
#include "d3d9/presentation/swap_chain_proxy.h"
#include "display/display_profile.h"
#include "game/core/game_profile.h"
#include "game/shared/timing/cutscene_terminal_frame.h"
#include "game/titles/ff13/ui/prompts/prompt_mode_manager.h"
#include "overlay/overlay.h"
#include "settings/graphics_settings.h"

#include <windows.h>
#include <d3d9.h>


namespace novafix::d3d9 {
namespace {

using namespace device_registry;

HRESULT STDMETHODCALLTYPE DevicePresentHook(IDirect3DDevice9* self, const RECT* source, const RECT* destination,
                                             HWND overrideWindow, const RGNDATA* dirty) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.d3d9.device-present-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.d3d9.device-present-original");
    hook_profiler::Scope totalTiming(totalPoint);
    const auto& functions = FindDeviceVtable(self);
    if (!functions.present) return D3DERR_INVALIDCALL;
    const DeviceRecord record = FindDevice(self);
    const bool owner = presentation::Begin(self, record.window, presentation::Path::Device);
    if (owner) mip_lod_bias::ApplyPendingState(self);
    const bool held = owner &&
        game::cutscene_terminal_frame::ShouldSuppressPresent();
    HRESULT result = D3D_OK;
    if (!held) {
        hook_profiler::Scope originalTiming(originalPoint);
        result = functions.present(
            self, source, destination, overrideWindow, dirty);
    }
    if (owner) presentation::End(result);
    return result;
}

HRESULT STDMETHODCALLTYPE DevicePresentExHook(
    IDirect3DDevice9Ex* self, const RECT* source, const RECT* destination,
    HWND overrideWindow, const RGNDATA* dirty, DWORD flags) {
    static const auto totalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.d3d9.device-present-ex-total");
    static const auto originalPoint = hook_profiler::RegisterDynamicPoint(
        "hook.d3d9.device-present-ex-original");
    hook_profiler::Scope totalTiming(totalPoint);
    const auto& functions = FindDeviceVtable(self);
    if (!functions.presentEx) return D3DERR_INVALIDCALL;
    const DeviceRecord record = FindDevice(self);
    const bool owner =
        presentation::Begin(self, record.window, presentation::Path::Device);
    if (owner) mip_lod_bias::ApplyPendingState(self);
    const bool held = owner &&
        game::cutscene_terminal_frame::ShouldSuppressPresent();
    HRESULT result = D3D_OK;
    if (!held) {
        hook_profiler::Scope originalTiming(originalPoint);
        result = functions.presentEx(
            self, source, destination, overrideWindow, dirty, flags);
    }
    if (owner) presentation::End(result);
    return result;
}

ULONG STDMETHODCALLTYPE DeviceReleaseHook(IDirect3DDevice9* self) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.release) return 0;
    ULONG references = functions.release(self);
    if (references == 1 && overlay::OwnsDevice(self)) {
        // ImGui owns the sole remaining reference. Its shutdown performs the
        // final (nested) Release and the nested hook retires this device
        overlay::DetachDevice(self);
        return 0;
    } else if (references == 0) {
        overlay::DetachDevice(self);
    }
    if (references == 0) {
        redundant_state_filter::Invalidate(self);
        DeactivateDevice(self);
    }
    return references;
}

HRESULT STDMETHODCALLTYPE SetScissorRectHook(IDirect3DDevice9* self, const RECT* rectangle) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.d3d9.set-scissor-rect-total", 64u);
    const auto& functions = FindDeviceVtable(self);
    return device_ui_hooks::SetScissorRectHook(
        self, rectangle,
        {functions.setScissorRect, functions.createVertexBuffer,
         functions.drawPrimitiveUP},
        [](IDirect3DDevice9* device) {
            const DeviceRecord record = FindDevice(device);
            return device_ui_hooks::DeviceExtent{
                record.backBufferWidth, record.backBufferHeight};
        });
}

HRESULT STDMETHODCALLTYPE CreateVertexBufferHook(
    IDirect3DDevice9* self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9** output, HANDLE* shared) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.d3d9.create-vertex-buffer-total");
    const auto& functions = FindDeviceVtable(self);
    return device_ui_hooks::CreateVertexBufferHook(
        self, length, usage, fvf, pool, output, shared,
        {functions.setScissorRect, functions.createVertexBuffer,
         functions.drawPrimitiveUP});
}

HRESULT STDMETHODCALLTYPE DrawPrimitiveUPHook(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
    const void* vertexData, UINT stride) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.d3d9.draw-primitive-up-total", 64u);
    const auto& functions = FindDeviceVtable(self);
    return device_ui_hooks::DrawPrimitiveUPHook(
        self, primitiveType, primitiveCount, vertexData, stride,
        {functions.setScissorRect, functions.createVertexBuffer,
         functions.drawPrimitiveUP});
}

HRESULT STDMETHODCALLTYPE DeviceResetHook(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.reset) return D3DERR_INVALIDCALL;

    const DeviceRecord record = FindDevice(self);
    const bool conservativeRecovery =
        game::CurrentProfile().title == game::Title::FinalFantasyXIII;
    const HRESULT cooperativeResult = conservativeRecovery
        ? self->TestCooperativeLevel() : D3D_OK;
    const bool preserveRecoveryParameters =
        device_recovery::SuspendNovaFixWork(
            conservativeRecovery, cooperativeResult);
    if (!preserveRecoveryParameters) {
        if (record.forcedProfile && compat::compatibility::AllowsNovaFixChanges()) {
            display::ApplyProfile(record.window, parameters);
        }
        presentation_setup::ApplyDevicePolicy(self, parameters);
    } else {
        // GRC::OnSwap_ already supplies profiled swap-chain parameters
        // Do not rebuild them through IDirect3D9 during XIII device recovery
        Log("XIII recovery Reset preserving swap-chain parameters: cooperative=0x%08lX",
            static_cast<unsigned long>(cooperativeResult));
    }
    game::cutscene_terminal_frame::BeforeDeviceReset();
    if (game::CurrentProfile().title == game::Title::FinalFantasyXIII) {
        game::ff13_prompt_mode::BeforeDeviceReset();
    }
    overlay::BeforeDeviceReset(self);
    mip_lod_bias::ResetSamplerCache();
    redundant_state_filter::Invalidate(self);
    const HRESULT result = functions.reset(self, parameters);
    overlay::AfterDeviceReset(self, result);
    if (SUCCEEDED(result)) UpdateDevicePresentation(self, parameters);
    Log("IDirect3DDevice9::Reset device=%p result=0x%08lX size=%ux%u", self,
        static_cast<unsigned long>(result), parameters ? parameters->BackBufferWidth : 0,
        parameters ? parameters->BackBufferHeight : 0);
    return result;
}

HRESULT STDMETHODCALLTYPE DeviceResetExHook(
    IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* fullscreenMode) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.resetEx) return D3DERR_INVALIDCALL;

    const DeviceRecord record = FindDevice(self);
    if (record.forcedProfile &&
        compat::compatibility::AllowsNovaFixChanges()) {
        display::ApplyProfile(record.window, parameters);
    }
    presentation_setup::ApplyDevicePolicy(self, parameters);

    IDirect3D9* d3d = nullptr;
    IDirect3D9Ex* d3dEx = nullptr;
    if (SUCCEEDED(self->GetDirect3D(&d3d)) && d3d) {
        d3d->QueryInterface(
            IID_IDirect3D9Ex, reinterpret_cast<void**>(&d3dEx));
        d3d->Release();
    }
    D3DDEVICE_CREATION_PARAMETERS creation{};
    self->GetCreationParameters(&creation);
    D3DDISPLAYMODEEX adjustedMode{};
    D3DDISPLAYMODEEX* effectiveMode =
        presentation_setup::ResolveFullscreenMode(
            d3dEx, creation.AdapterOrdinal, parameters, fullscreenMode,
            adjustedMode);

    game::cutscene_terminal_frame::BeforeDeviceReset();
    if (game::CurrentProfile().title == game::Title::FinalFantasyXIII) {
        game::ff13_prompt_mode::BeforeDeviceReset();
    }
    overlay::BeforeDeviceReset(self);
    mip_lod_bias::ResetSamplerCache();
    redundant_state_filter::Invalidate(self);
    const HRESULT result =
        functions.resetEx(self, parameters, effectiveMode);
    overlay::AfterDeviceReset(self, result);
    if (SUCCEEDED(result)) {
        UpdateDevicePresentation(self, parameters);
        if (fullscreenMode && effectiveMode) {
            *fullscreenMode = *effectiveMode;
        }
    }
    if (d3dEx) d3dEx->Release();
    Log("IDirect3DDevice9Ex::ResetEx device=%p result=0x%08lX size=%ux%u",
        self, static_cast<unsigned long>(result),
        parameters ? parameters->BackBufferWidth : 0,
        parameters ? parameters->BackBufferHeight : 0);
    return result;
}

HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChainHook(IDirect3DDevice9* self,
                                                         D3DPRESENT_PARAMETERS* parameters,
                                                         IDirect3DSwapChain9** output) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.createAdditionalSwapChain) return D3DERR_INVALIDCALL;
    const DeviceRecord record = FindDevice(self);
    return swap_chain::Wrap(functions.createAdditionalSwapChain(self, parameters, output),
                            output, self, record.window, record.extended);
}

HRESULT STDMETHODCALLTYPE GetSwapChainHook(IDirect3DDevice9* self, UINT index,
                                            IDirect3DSwapChain9** output) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.getSwapChain) return D3DERR_INVALIDCALL;
    const DeviceRecord record = FindDevice(self);
    return swap_chain::Wrap(functions.getSwapChain(self, index, output),
                            output, self, record.window, record.extended);
}

void RegisterDeviceInternal(
    IDirect3DDevice9* device, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended) {
    if (!device) return;
    void** table = *reinterpret_cast<void***>(device);
    const DeviceHookPolicy hookPolicy =
        device_hook_installer::ConfigureDeviceHookPolicy();
    static const device_hook_installer::EntryPoints entryPoints{
        &DeviceReleaseHook,
        &CreateAdditionalSwapChainHook,
        &GetSwapChainHook,
        &DeviceResetHook,
        &DevicePresentHook,
        &SetScissorRectHook,
        &CreateVertexBufferHook,
        &DrawPrimitiveUPHook,
        &DevicePresentExHook,
        &DeviceResetExHook,
    };

    device_registry::RegisterDevice(
        device, table, window, forcedProfile, parameters, extended, hookPolicy,
        [](IDirect3DDevice9* device, void** table, DeviceVtableRecord* record,
           const DeviceHookPolicy& hookPolicy, bool extended) {
            device_hook_installer::Install(
                device, table, record, hookPolicy, extended, entryPoints);
        });
}

} // namespace

namespace device_hooks {

void Register(
    IDirect3DDevice9* device, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended) {
    RegisterDeviceInternal(
        device, window, forcedProfile, parameters, extended);
}

void PreviewXiii2MipLodBias(float value) {
    mip_lod_bias::Preview(settings::Active().anisotropicFiltering, value);
}

void PreviewTextureFiltering(unsigned anisotropy, float mipLodBias) {
    mip_lod_bias::Preview(anisotropy, mipLodBias);
}

} // namespace device_hooks

} // namespace novafix::d3d9
