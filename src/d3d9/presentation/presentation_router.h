#pragma once

#include <windows.h>

struct IDirect3DDevice9;

namespace novafix::d3d9::presentation {

enum class Path { Device, SwapChain };

bool Begin(IDirect3DDevice9* device, HWND window, Path path);
void End(HRESULT result);

} // namespace novafix::d3d9::presentation
