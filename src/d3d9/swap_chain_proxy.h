#pragma once

#include <windows.h>

struct IDirect3DDevice9;
struct IDirect3DSwapChain9;

namespace novafix::d3d9::swap_chain {

HRESULT Wrap(HRESULT result, IDirect3DSwapChain9** output,
             IDirect3DDevice9* device, HWND window, bool extended);

} // namespace novafix::d3d9::swap_chain
