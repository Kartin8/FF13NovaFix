#pragma once

#include <windows.h>
#include <d3d9.h>

namespace novafix::d3d9::device_hooks {

void Register(
    IDirect3DDevice9* device, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended);
void PreviewXiii2MipLodBias(float value);
void PreviewTextureFiltering(unsigned anisotropy, float mipLodBias);

} // namespace novafix::d3d9::device_hooks
