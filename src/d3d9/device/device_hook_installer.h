#pragma once

#include "d3d9/device/device_hook_policy.h"
#include "d3d9/device/device_registry.h"

namespace novafix::d3d9::device_hook_installer {

struct EntryPoints {
    device_registry::DeviceReleaseFn release{};
    device_registry::CreateAdditionalSwapChainFn createAdditionalSwapChain{};
    device_registry::GetSwapChainFn getSwapChain{};
    device_registry::DeviceResetFn reset{};
    device_registry::DevicePresentFn present{};
    device_registry::SetScissorRectFn setScissorRect{};
    device_registry::CreateVertexBufferFn createVertexBuffer{};
    device_registry::DrawPrimitiveUPFn drawPrimitiveUP{};
    device_registry::DevicePresentExFn presentEx{};
    device_registry::DeviceResetExFn resetEx{};
};

DeviceHookPolicy ConfigureDeviceHookPolicy();

// Called under the device registry's exclusive lock.
void Install(
    IDirect3DDevice9* device, void** table,
    device_registry::DeviceVtableRecord* record,
    const DeviceHookPolicy& hookPolicy, bool extended,
    const EntryPoints& entryPoints);

} // namespace novafix::d3d9::device_hook_installer
