#include "game/titles/ff13/rendering/nautilus_crash_fix.h"

#include "patch/iat_hook.h"
#include "diagnostics/log.h"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>

namespace novafix::game::ff13_nautilus_crash_fix {
namespace {

using D3DXCreateTextureFn = HRESULT (WINAPI*)(
    IDirect3DDevice9* device, UINT width, UINT height, UINT mipLevels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DTexture9** texture);

constexpr std::uint32_t kDxt1 = 0x31545844u;
constexpr std::uint32_t kManagedPool = 1u;

bool IsNautilusMovieTexture(
    std::uint32_t width, std::uint32_t height,
    std::uint32_t mipLevels, std::uint32_t usage,
    std::uint32_t format, std::uint32_t pool) {
    return width == 1280u && height == 720u && mipLevels == 1u &&
           usage == 0u && format == kDxt1 && pool == kManagedPool;
}

INIT_ONCE g_installOnce = INIT_ONCE_STATIC_INIT;
D3DXCreateTextureFn g_createTexture{};
std::atomic_bool g_allocationFailureActive{};

bool IsAllocationFailure(HRESULT result) {
    return result == D3DERR_OUTOFVIDEOMEMORY || result == E_OUTOFMEMORY;
}

HRESULT CreateDirect(
    IDirect3DDevice9* device, UINT width, UINT height, UINT mipLevels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DTexture9** texture) {
    if (!device || !texture) return D3DERR_INVALIDCALL;
    *texture = nullptr;
    return device->CreateTexture(
        width, height, mipLevels, usage, format, pool, texture, nullptr);
}

void LogTextureCapabilities(IDirect3DDevice9* device,
                            D3DFORMAT requestedFormat) {
    if (!device) return;

    D3DCAPS9 caps{};
    D3DDEVICE_CREATION_PARAMETERS creation{};
    const HRESULT capsResult = device->GetDeviceCaps(&caps);
    const HRESULT creationResult = device->GetCreationParameters(&creation);

    HRESULT displayResult = D3DERR_INVALIDCALL;
    HRESULT requestedSupport = D3DERR_INVALIDCALL;
    HRESULT argbSupport = D3DERR_INVALIDCALL;
    D3DDISPLAYMODE display{};
    IDirect3D9* direct3D{};
    if (SUCCEEDED(device->GetDirect3D(&direct3D)) && direct3D &&
        SUCCEEDED(creationResult)) {
        displayResult = direct3D->GetAdapterDisplayMode(
            creation.AdapterOrdinal, &display);
        if (SUCCEEDED(displayResult)) {
            requestedSupport = direct3D->CheckDeviceFormat(
                creation.AdapterOrdinal, creation.DeviceType, display.Format,
                0, D3DRTYPE_TEXTURE, requestedFormat);
            argbSupport = direct3D->CheckDeviceFormat(
                creation.AdapterOrdinal, creation.DeviceType, display.Format,
                0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
        }
        direct3D->Release();
    }

    Log("XIII Nautilus texture caps: caps=0x%08lX create=0x%08lX "
        "display=0x%08lX texture-caps=0x%08lX max=%ux%u "
        "display-format=0x%08X requested-support=0x%08lX "
        "argb-support=0x%08lX",
        static_cast<unsigned long>(capsResult),
        static_cast<unsigned long>(creationResult),
        static_cast<unsigned long>(displayResult),
        static_cast<unsigned long>(caps.TextureCaps), caps.MaxTextureWidth,
        caps.MaxTextureHeight, static_cast<unsigned>(display.Format),
        static_cast<unsigned long>(requestedSupport),
        static_cast<unsigned long>(argbSupport));
}

HRESULT WINAPI D3DXCreateTextureHook(
    IDirect3DDevice9* device, UINT width, UINT height, UINT mipLevels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DTexture9** texture) {
    if (!g_createTexture) return D3DERR_INVALIDCALL;

    const bool nautilusMovieTexture = IsNautilusMovieTexture(
        width, height, mipLevels, usage,
        static_cast<std::uint32_t>(format),
        static_cast<std::uint32_t>(pool));
    const HRESULT nativeResult = g_createTexture(
        device, width, height, mipLevels, usage, format, pool, texture);
    if (SUCCEEDED(nativeResult) || !texture || *texture ||
        !nautilusMovieTexture) {
        if (nautilusMovieTexture && SUCCEEDED(nativeResult)) {
            if (g_allocationFailureActive.exchange(
                    false, std::memory_order_acq_rel)) {
                Log("XIII Nautilus movie texture allocation recovered through the native path");
            }
        }
        return nativeResult;
    }

    const bool reportFailure = !g_allocationFailureActive.exchange(
        true, std::memory_order_acq_rel);
    if (reportFailure) {
        Log("XIII Nautilus movie texture allocation failed: hr=0x%08lX "
            "size=%ux%u levels=%u usage=0x%08lX format=0x%08X pool=%u; repeats suppressed until recovery",
            static_cast<unsigned long>(nativeResult), width, height,
            mipLevels, static_cast<unsigned long>(usage),
            static_cast<unsigned>(format), static_cast<unsigned>(pool));
        LogTextureCapabilities(device, format);
    }

    // Free GPU copies of managed resources before giving up. Their system-memory
    // copies remain available for Direct3D to upload again
    if (device && IsAllocationFailure(nativeResult)) {
        const HRESULT evictResult = device->EvictManagedResources();
        IDirect3DTexture9* retried{};
        const HRESULT retryResult = g_createTexture(
            device, width, height, mipLevels, usage, format, pool, &retried);
        if (reportFailure) {
            Log("XIII Nautilus managed-resource retry: evict=0x%08lX "
                "retry=0x%08lX texture=%p",
                static_cast<unsigned long>(evictResult),
                static_cast<unsigned long>(retryResult), retried);
        }
        if (SUCCEEDED(retryResult) && retried) {
            g_allocationFailureActive.store(
                false, std::memory_order_release);
            if (!reportFailure) {
                Log("XIII Nautilus movie texture allocation recovered through the managed-resource retry");
            }
            *texture = retried;
            return retryResult;
        }
        if (retried) retried->Release();
    }

    IDirect3DTexture9* directManaged{};
    const HRESULT directManagedResult = CreateDirect(
        device, width, height, mipLevels, usage, format, pool,
        &directManaged);
    if (SUCCEEDED(directManagedResult) && directManaged) {
        g_allocationFailureActive.store(false, std::memory_order_release);
        Log("XIII Nautilus movie texture recovered through direct managed "
            "creation: texture=%p", directManaged);
        *texture = directManaged;
        return directManagedResult;
    }
    if (directManaged) directManaged->Release();

    // If DXT1 fails, let D3DX decode to ARGB at 1280x720 to preserve movie UVs
    // Keep MANAGED: this hook does not own the texture's Reset lifetime
    IDirect3DTexture9* argbManaged{};
    const HRESULT argbManagedResult = CreateDirect(
        device, width, height, mipLevels, usage, D3DFMT_A8R8G8B8, pool,
        &argbManaged);
    if (SUCCEEDED(argbManagedResult) && argbManaged) {
        g_allocationFailureActive.store(false, std::memory_order_release);
        Log("XIII Nautilus movie texture recovered through ARGB managed "
            "creation: texture=%p", argbManaged);
        *texture = argbManaged;
        return argbManagedResult;
    }
    if (argbManaged) argbManaged->Release();

    return nativeResult;
}

BOOL CALLBACK InstallOnce(PINIT_ONCE, PVOID, PVOID*) {
    const bool installed = PatchExecutableImport(
        "d3dx9_43.dll", "D3DXCreateTexture",
        reinterpret_cast<void*>(&D3DXCreateTextureHook),
        reinterpret_cast<void**>(&g_createTexture));
    if (!installed) {
        LogWarning("XIII Nautilus movie crash fix could not be installed");
    }
    return TRUE;
}

} // namespace

void Install() {
    InitOnceExecuteOnce(&g_installOnce, &InstallOnce, nullptr, nullptr);
}

} // namespace novafix::game::ff13_nautilus_crash_fix
