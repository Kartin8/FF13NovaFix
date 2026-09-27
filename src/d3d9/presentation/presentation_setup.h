#pragma once

#include <windows.h>
#include <d3d9.h>

namespace novafix::d3d9::presentation_setup {

struct DeviceCreationState {
    UINT requestedWidth{};
    UINT requestedHeight{};
    BOOL requestedWindowed{};
    HWND window{};
    bool forcedProfile{};
};

DeviceCreationState PrepareDeviceCreation(
    IDirect3D9* d3d, UINT adapter, HWND focusWindow,
    D3DPRESENT_PARAMETERS* parameters);

void ApplyDevicePolicy(
    IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters);

D3DDISPLAYMODEEX* ResolveFullscreenMode(
    IDirect3D9Ex* d3d, UINT adapter,
    const D3DPRESENT_PARAMETERS* parameters,
    const D3DDISPLAYMODEEX* requested,
    D3DDISPLAYMODEEX& storage);

} // namespace novafix::d3d9::presentation_setup
