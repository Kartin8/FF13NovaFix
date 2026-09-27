#pragma once

#include <windows.h>
#include <d3d9.h>

namespace novafix::d3d9::device_ui_hooks {

struct Originals {
    HRESULT (STDMETHODCALLTYPE* setScissorRect)(IDirect3DDevice9*, const RECT*){};
    HRESULT (STDMETHODCALLTYPE* createVertexBuffer)(
        IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
        IDirect3DVertexBuffer9**, HANDLE*){};
    HRESULT (STDMETHODCALLTYPE* drawPrimitiveUP)(
        IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT){};
};

struct DeviceExtent {
    UINT backBufferWidth{};
    UINT backBufferHeight{};
};

HRESULT STDMETHODCALLTYPE SetScissorRectHook(
    IDirect3DDevice9* self, const RECT* rectangle,
    const Originals& functions,
    DeviceExtent (*findExtent)(IDirect3DDevice9*));
HRESULT STDMETHODCALLTYPE CreateVertexBufferHook(
    IDirect3DDevice9* self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9** output, HANDLE* shared, const Originals& functions);
HRESULT STDMETHODCALLTYPE DrawPrimitiveUPHook(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
    const void* vertexData, UINT stride, const Originals& functions);

} // namespace novafix::d3d9::device_ui_hooks
