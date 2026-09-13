#include "d3d9/factory_hooks.h"

#include "diagnostics/log.h"
#include "patch/vtable_hook.h"
#include "d3d9/device_hooks.h"
#include "d3d9/presentation_setup.h"
#include "game/core/features.h"
#include "game/core/game_profile.h"
#include "game/shared/ui/startup_module_notification.h"

#include <windows.h>
#include <d3d9.h>

#include <vector>

namespace novafix::d3d9 {
namespace {

using CreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using CreateDeviceExFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

struct FactoryVtable {
    void** vtable{};
    CreateDeviceFn createDevice{};
    CreateDeviceExFn createDeviceEx{};
    patch::registry::ClaimId createDeviceClaim{};
    patch::registry::ClaimId createDeviceExClaim{};
};

SRWLOCK g_factoryLock = SRWLOCK_INIT;
std::vector<FactoryVtable> g_factoryVtables;

CreateDeviceFn FindCreateDevice(IDirect3D9* d3d) {
    if (!d3d) return nullptr;
    void** const table = *reinterpret_cast<void***>(d3d);
    CreateDeviceFn result{};
    AcquireSRWLockShared(&g_factoryLock);
    for (const FactoryVtable& record : g_factoryVtables) {
        if (record.vtable == table) {
            result = record.createDevice;
            break;
        }
    }
    ReleaseSRWLockShared(&g_factoryLock);
    return result;
}

CreateDeviceExFn FindCreateDeviceEx(IDirect3D9Ex* d3d) {
    if (!d3d) return nullptr;
    void** const table = *reinterpret_cast<void***>(d3d);
    CreateDeviceExFn result{};
    AcquireSRWLockShared(&g_factoryLock);
    for (const FactoryVtable& record : g_factoryVtables) {
        if (record.vtable == table) {
            result = record.createDeviceEx;
            break;
        }
    }
    ReleaseSRWLockShared(&g_factoryLock);
    return result;
}

void CompleteDeviceCreation(
    const char* operation, HRESULT result, IDirect3DDevice9* device,
    const presentation_setup::DeviceCreationState& state,
    const D3DPRESENT_PARAMETERS* parameters, bool extended) {
    Log("%s result=0x%08lX device=%p hwnd=%p requested=%ux%u windowed=%d actual=%ux%u",
        operation, static_cast<unsigned long>(result), device, state.window,
        state.requestedWidth, state.requestedHeight, state.requestedWindowed,
        parameters ? parameters->BackBufferWidth : 0,
        parameters ? parameters->BackBufferHeight : 0);
    if (FAILED(result) || !device) return;

    IDirect3DSurface9* backBuffer = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
        backBuffer) {
        D3DSURFACE_DESC description{};
        if (SUCCEEDED(backBuffer->GetDesc(&description))) {
            Log("%s verified real back buffer: %ux%u format=%u launcher=%ux%u windowed=%d",
                operation, description.Width, description.Height,
                static_cast<unsigned>(description.Format),
                state.requestedWidth, state.requestedHeight,
                state.requestedWindowed);
        }
        backBuffer->Release();
    }

    device_hooks::Register(
        device, state.window, state.forcedProfile, parameters, extended);
    game::startup_module_notification::RetryDeferredInstallation();
    game::features::Activate();
}

HRESULT STDMETHODCALLTYPE CreateDeviceHook(
    IDirect3D9* self, UINT adapter, D3DDEVTYPE type,
    HWND focusWindow, DWORD behavior,
    D3DPRESENT_PARAMETERS* parameters,
    IDirect3DDevice9** output) {
    const CreateDeviceFn createDevice = FindCreateDevice(self);
    if (!createDevice) return D3DERR_INVALIDCALL;

    const presentation_setup::DeviceCreationState state =
        presentation_setup::PrepareDeviceCreation(
            self, adapter, focusWindow, parameters);
    const HRESULT result = createDevice(
        self, adapter, type, focusWindow, behavior, parameters, output);
    CompleteDeviceCreation(
        "CreateDevice", result, output ? *output : nullptr,
        state, parameters, false);
    return result;
}

HRESULT STDMETHODCALLTYPE CreateDeviceExHook(
    IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type,
    HWND focusWindow, DWORD behavior,
    D3DPRESENT_PARAMETERS* parameters,
    D3DDISPLAYMODEEX* fullscreenMode,
    IDirect3DDevice9Ex** output) {
    const CreateDeviceExFn createDeviceEx = FindCreateDeviceEx(self);
    if (!createDeviceEx) return D3DERR_INVALIDCALL;

    const presentation_setup::DeviceCreationState state =
        presentation_setup::PrepareDeviceCreation(
            self, adapter, focusWindow, parameters);
    D3DDISPLAYMODEEX adjustedMode{};
    D3DDISPLAYMODEEX* effectiveMode =
        presentation_setup::ResolveFullscreenMode(
            self, adapter, parameters, fullscreenMode, adjustedMode);
    const HRESULT result = createDeviceEx(
        self, adapter, type, focusWindow, behavior, parameters,
        effectiveMode, output);
    if (SUCCEEDED(result) && fullscreenMode && effectiveMode) {
        *fullscreenMode = *effectiveMode;
    }
    CompleteDeviceCreation(
        "CreateDeviceEx", result, output ? *output : nullptr,
        state, parameters, true);
    return result;
}

} // namespace

void InstallFactoryHooks(IDirect3D9* d3d, bool extended) {
    if (!d3d) return;
    void** const table = *reinterpret_cast<void***>(d3d);

    AcquireSRWLockExclusive(&g_factoryLock);
    auto found = g_factoryVtables.end();
    for (auto current = g_factoryVtables.begin();
         current != g_factoryVtables.end(); ++current) {
        if (current->vtable == table) {
            found = current;
            break;
        }
    }
    if (found == g_factoryVtables.end()) {
        found = g_factoryVtables.emplace(
            g_factoryVtables.end(), FactoryVtable{table});
    }

    if (!found->createDevice) {
        void* original = nullptr;
        if (ReplaceVtableSlot(
                "d3d9:create-device", table, 16,
                reinterpret_cast<void*>(&CreateDeviceHook), &original,
                &found->createDeviceClaim)) {
            found->createDevice = reinterpret_cast<CreateDeviceFn>(original);
        }
    }
    if (extended && !found->createDeviceEx) {
        void* original = nullptr;
        if (ReplaceVtableSlot(
                "d3d9:create-device-ex", table, 20,
                reinterpret_cast<void*>(&CreateDeviceExHook), &original,
                &found->createDeviceExClaim)) {
            found->createDeviceEx =
                reinterpret_cast<CreateDeviceExFn>(original);
        }
    }
    ReleaseSRWLockExclusive(&g_factoryLock);
}

} // namespace novafix::d3d9
