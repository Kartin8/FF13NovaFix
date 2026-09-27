#pragma once

#include "d3d9/device/device_hook_policy.h"
#include "patch/registry.h"

#include <windows.h>
#include <d3d9.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace novafix::d3d9::device_registry {

using DeviceReleaseFn = ULONG (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using CreateAdditionalSwapChainFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*,
                                                                  IDirect3DSwapChain9**);
using GetSwapChainFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);
using DeviceResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using DevicePresentFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                                      const RGNDATA*);
using DeviceResetExFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using DevicePresentExFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using SetScissorRectFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*);
using CreateVertexBufferFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD,
                                                           D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using DrawPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
                                                        UINT, const void*, UINT);

struct DeviceRecord {
    IDirect3DDevice9* device{};
    void** vtable{};
    HWND window{};
    bool forcedProfile{};
    bool extended{};
    UINT backBufferWidth{};
    UINT backBufferHeight{};
};

enum class DeviceClaim : std::uint8_t {
    Release,
    CreateAdditionalSwapChain,
    GetSwapChain,
    Reset,
    Present,
    SetScissorRect,
    CreateVertexBuffer,
    DrawPrimitiveUp,
    PresentEx,
    ResetEx,
    Count,
};

constexpr std::size_t kDeviceClaimCount =
    static_cast<std::size_t>(DeviceClaim::Count);

struct DeviceVtableRecord {
    void** vtable{};
    DeviceReleaseFn release{};
    CreateAdditionalSwapChainFn createAdditionalSwapChain{};
    GetSwapChainFn getSwapChain{};
    DeviceResetFn reset{};
    DevicePresentFn present{};
    DeviceResetExFn resetEx{};
    DevicePresentExFn presentEx{};
    SetScissorRectFn setScissorRect{};
    CreateVertexBufferFn createVertexBuffer{};
    DrawPrimitiveUPFn drawPrimitiveUP{};
    std::array<patch::registry::ClaimId, kDeviceClaimCount> claims{};
};

inline patch::registry::ClaimId* Claim(
    DeviceVtableRecord& record, DeviceClaim claim) {
    return &record.claims[static_cast<std::size_t>(claim)];
}

DeviceRecord FindDevice(IDirect3DDevice9* device);
const DeviceVtableRecord& FindDeviceVtable(IDirect3DDevice9* device);
void DeactivateDevice(IDirect3DDevice9* device);
void UpdateDevicePresentation(
    IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* parameters);

// Invoked under the registry's exclusive lock. Must not re-enter the registry.
using InstallHooksFn = void (*)(
    IDirect3DDevice9* device, void** table, DeviceVtableRecord* record,
    const DeviceHookPolicy& hookPolicy, bool extended);

void RegisterDevice(
    IDirect3DDevice9* device, void** table, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended,
    const DeviceHookPolicy& hookPolicy, InstallHooksFn installHooks);

} // namespace novafix::d3d9::device_registry
