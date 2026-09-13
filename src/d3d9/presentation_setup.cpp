#include "d3d9/presentation_setup.h"

#include "diagnostics/log.h"
#include "compat/compatibility.h"
#include "display/display_profile.h"
#include "display/presentation_policy.h"
#include "game/core/game_profile.h"
#include "settings/performance_settings.h"

namespace novafix::d3d9::presentation_setup {
namespace {

D3DFORMAT AdapterFormat(IDirect3D9* d3d, UINT adapter) {
    D3DDISPLAYMODE mode{};
    return d3d && SUCCEEDED(d3d->GetAdapterDisplayMode(adapter, &mode))
        ? mode.Format : D3DFMT_UNKNOWN;
}

unsigned ResolveRefreshRate(
    IDirect3D9* d3d, UINT adapter,
    const D3DPRESENT_PARAMETERS& parameters, unsigned requested) {
    if (!d3d || parameters.Windowed || requested == 0) return 0;

    D3DFORMAT format = parameters.BackBufferFormat;
    if (format == D3DFMT_UNKNOWN) format = AdapterFormat(d3d, adapter);
    if (format == D3DFMT_UNKNOWN) return 0;

    const UINT count = d3d->GetAdapterModeCount(adapter, format);
    for (UINT index = 0; index < count; ++index) {
        D3DDISPLAYMODE mode{};
        if (SUCCEEDED(d3d->EnumAdapterModes(adapter, format, index, &mode)) &&
            mode.Width == parameters.BackBufferWidth &&
            mode.Height == parameters.BackBufferHeight &&
            mode.RefreshRate == requested) {
            return requested;
        }
    }

    Log("Requested exclusive refresh rate unavailable: %ux%u@%u using=adapter-default",
        parameters.BackBufferWidth, parameters.BackBufferHeight, requested);
    return 0;
}

void ApplyRuntimePolicy(
    IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* parameters) {
    if (!parameters || parameters->BackBufferWidth < 640 ||
        parameters->BackBufferHeight < 480 ||
        !compat::compatibility::AllowsNovaFixChanges()) {
        return;
    }

    if (game::CurrentProfile().title == game::Title::LightningReturns) {
        const UINT originalInterval = parameters->PresentationInterval;
        parameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        Log("LR VSync bypass applied: interval=%u->%u buffers=%u swap=%u refresh=%u",
            originalInterval, parameters->PresentationInterval,
            parameters->BackBufferCount,
            static_cast<unsigned>(parameters->SwapEffect),
            parameters->FullScreen_RefreshRateInHz);
        return;
    }

    const settings::PerformanceSettings profile =
        settings::ActivePerformance();
    const UINT oldInterval = parameters->PresentationInterval;
    const UINT oldBuffers = parameters->BackBufferCount;
    const D3DSWAPEFFECT oldSwap = parameters->SwapEffect;
    const unsigned refresh = ResolveRefreshRate(
        d3d, adapter, *parameters, profile.refreshRate);
    display::ApplyPresentationPolicy(*parameters, profile, {refresh});
    Log("Presentation policy applied: windowed=%d interval=%u->%u buffers=%u->%u swap=%u->%u refresh=%u",
        parameters->Windowed, oldInterval, parameters->PresentationInterval,
        oldBuffers, parameters->BackBufferCount,
        static_cast<unsigned>(oldSwap),
        static_cast<unsigned>(parameters->SwapEffect),
        parameters->FullScreen_RefreshRateInHz);
}

} // namespace

DeviceCreationState PrepareDeviceCreation(
    IDirect3D9* d3d, UINT adapter, HWND focusWindow,
    D3DPRESENT_PARAMETERS* parameters) {
    DeviceCreationState state{
        parameters ? parameters->BackBufferWidth : 0,
        parameters ? parameters->BackBufferHeight : 0,
        parameters ? parameters->Windowed : FALSE,
        display::PresentationWindow(focusWindow, parameters),
        false,
    };
    state.forcedProfile =
        compat::compatibility::AllowsNovaFixChanges() &&
        game::Supports(game::Capability::PresentationOverride) &&
        display::ShouldApplyProfile(parameters);
    if (state.forcedProfile) {
        display::ApplyProfile(focusWindow, parameters);
    }
    ApplyRuntimePolicy(d3d, adapter, parameters);
    return state;
}

void ApplyDevicePolicy(
    IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
    if (!device || !parameters) return;

    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(device->GetCreationParameters(&creation))) return;

    IDirect3D9* d3d = nullptr;
    if (FAILED(device->GetDirect3D(&d3d)) || !d3d) return;
    ApplyRuntimePolicy(d3d, creation.AdapterOrdinal, parameters);
    d3d->Release();
}

D3DDISPLAYMODEEX* ResolveFullscreenMode(
    IDirect3D9Ex* d3d, UINT adapter,
    const D3DPRESENT_PARAMETERS* parameters,
    const D3DDISPLAYMODEEX* requested,
    D3DDISPLAYMODEEX& storage) {
    if (!parameters || parameters->Windowed) return nullptr;

    storage = {};
    storage.Size = sizeof(storage);
    if (requested && requested->Size == sizeof(*requested)) {
        storage = *requested;
    } else if (!d3d ||
               FAILED(d3d->GetAdapterDisplayModeEx(
                   adapter, &storage, nullptr))) {
        storage.Size = sizeof(storage);
        storage.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }

    storage.Size = sizeof(storage);
    if (parameters->BackBufferWidth) {
        storage.Width = parameters->BackBufferWidth;
    }
    if (parameters->BackBufferHeight) {
        storage.Height = parameters->BackBufferHeight;
    }
    if (parameters->BackBufferFormat != D3DFMT_UNKNOWN) {
        storage.Format = parameters->BackBufferFormat;
    }
    if (parameters->FullScreen_RefreshRateInHz) {
        storage.RefreshRate = parameters->FullScreen_RefreshRateInHz;
    }
    return &storage;
}

} // namespace novafix::d3d9::presentation_setup
