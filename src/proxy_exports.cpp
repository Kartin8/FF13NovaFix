#include <windows.h>

#define Direct3DCreate9 SystemHeaderDirect3DCreate9
#define Direct3DCreate9Ex SystemHeaderDirect3DCreate9Ex
#define D3DPERF_BeginEvent SystemHeaderD3DPERF_BeginEvent
#define D3DPERF_EndEvent SystemHeaderD3DPERF_EndEvent
#define D3DPERF_GetStatus SystemHeaderD3DPERF_GetStatus
#define D3DPERF_QueryRepeatFrame SystemHeaderD3DPERF_QueryRepeatFrame
#define D3DPERF_SetMarker SystemHeaderD3DPERF_SetMarker
#define D3DPERF_SetOptions SystemHeaderD3DPERF_SetOptions
#define D3DPERF_SetRegion SystemHeaderD3DPERF_SetRegion
#include <d3d9.h>
#undef Direct3DCreate9
#undef Direct3DCreate9Ex
#undef D3DPERF_BeginEvent
#undef D3DPERF_EndEvent
#undef D3DPERF_GetStatus
#undef D3DPERF_QueryRepeatFrame
#undef D3DPERF_SetMarker
#undef D3DPERF_SetOptions
#undef D3DPERF_SetRegion

#define Direct3DCreate9On12 SystemHeaderDirect3DCreate9On12
#define Direct3DCreate9On12Ex SystemHeaderDirect3DCreate9On12Ex
#if __has_include(<d3d9on12.h>)
#include <d3d9on12.h>
#else
struct D3D9ON12_ARGS;
#endif
#undef Direct3DCreate9On12
#undef Direct3DCreate9On12Ex

#include "patch/iat_hook.h"
#include "diagnostics/log.h"
#include "common/module_path.h"
#include "compat/addons/addon_loader.h"
#include "d3d9/backend.h"
#include "d3d9/factory_hooks.h"
#include "overlay/overlay.h"
#include "patch/memory_access.h"
#include "runtime/bootstrap.h"
#include "runtime/restart.h"

namespace {

struct ExternalDispatch {
    bool intercepted{};
    void* target{};
};

bool ExternalCallable(void* target, void* proxyExport,
                      std::wstring& ownerPath) {
    if (!target || target == proxyExport ||
        !novafix::patch::memory::IsExecutable(target, 1u)) {
        return false;
    }
    HMODULE owner{};
    HMODULE proxy{};
    constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(target), &owner) ||
        !GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(proxyExport), &proxy) ||
        !owner || owner == proxy) {
        return false;
    }
    ownerPath = novafix::path::ModuleFile(owner);
    return !ownerPath.empty();
}

ExternalDispatch ResolveNewAddonDispatch(const char* exportName,
                                         void* proxyExport) {
    void* imported = novafix::ReadExecutableImportTarget(
        "d3d9.dll", exportName);
    std::wstring ownerPath;
    if (ExternalCallable(imported, proxyExport, ownerPath)) {
        Log("D3D export resolves to add-on: %s owner=%ls",
            exportName, ownerPath.c_str());
        return {true, imported};
    }

    void* resolverTarget = novafix::ReadExecutableImportTarget(
        "kernel32.dll", "GetProcAddress");
    std::wstring resolverOwner;
    if (!ExternalCallable(
            resolverTarget, reinterpret_cast<void*>(&GetProcAddress),
            resolverOwner)) {
        return {};
    }

    HMODULE proxy{};
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(proxyExport), &proxy);
    using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
    FARPROC resolved = reinterpret_cast<GetProcAddressFn>(resolverTarget)(
        proxy, exportName);
    if (!resolved) {
        LogWarning("Add-on resolver returned no target: %s owner=%ls",
            exportName, resolverOwner.c_str());
        return {};
    }

    ownerPath.clear();
    if (!ExternalCallable(
            reinterpret_cast<void*>(resolved), proxyExport, ownerPath)) {
        return {};
    }
    Log("D3D export resolved via add-on resolver: %s owner=%ls",
        exportName, ownerPath.c_str());
    return {true, reinterpret_cast<void*>(resolved)};
}

bool AddonsLoadedByCurrentCall(bool initializedHere) {
    return initializedHere &&
        novafix::compat::addon_loader::LoadedCount() != 0u;
}

void HookFactory(IDirect3D9* d3d, bool extended) {
    if (novafix::runtime::ShouldInstallD3D9Hooks()) {
        novafix::d3d9::InstallFactoryHooks(d3d, extended);
    }
}

template <typename Function>
Function BackendExport(const char* name) {
    novafix::runtime::EnsureInitialized();
    return reinterpret_cast<Function>(novafix::d3d9::backend::Export(name));
}

template <typename Function>
Function BackendOrdinal(WORD ordinal) {
    novafix::runtime::EnsureInitialized();
    return reinterpret_cast<Function>(
        novafix::d3d9::backend::ExportOrdinal(ordinal));
}

} // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    using Function = IDirect3D9* (WINAPI*)(UINT);
    const bool initializedHere = novafix::runtime::EnsureInitialized();
    if (AddonsLoadedByCurrentCall(initializedHere)) {
        const ExternalDispatch dispatch = ResolveNewAddonDispatch(
            "Direct3DCreate9", reinterpret_cast<void*>(&Direct3DCreate9));
        if (dispatch.intercepted) {
            const auto outer = reinterpret_cast<Function>(dispatch.target);
            IDirect3D9* d3d = outer ? outer(sdkVersion) : nullptr;
            HookFactory(d3d, false);
            return d3d;
        }
    }
    const auto function = BackendExport<Function>("Direct3DCreate9");
    IDirect3D9* d3d = function ? function(sdkVersion) : nullptr;
    HookFactory(d3d, false);
    return d3d;
}

extern "C" void WINAPI Direct3D9ForceHybridEnumeration(UINT enabled) {
    using Function = void (WINAPI*)(UINT);
    const auto function = BackendOrdinal<Function>(16);
    if (function) function(enabled);
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** output) {
    using Function = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
    const bool initializedHere = novafix::runtime::EnsureInitialized();
    if (AddonsLoadedByCurrentCall(initializedHere)) {
        const ExternalDispatch dispatch = ResolveNewAddonDispatch(
            "Direct3DCreate9Ex", reinterpret_cast<void*>(&Direct3DCreate9Ex));
        if (dispatch.intercepted) {
            const auto outer = reinterpret_cast<Function>(dispatch.target);
            const HRESULT result = outer
                ? outer(sdkVersion, output) : D3DERR_NOTAVAILABLE;
            if (SUCCEEDED(result) && output) HookFactory(*output, true);
            return result;
        }
    }
    const auto function = BackendExport<Function>("Direct3DCreate9Ex");
    if (!function) return D3DERR_NOTAVAILABLE;
    const HRESULT result = function(sdkVersion, output);
    if (SUCCEEDED(result) && output) HookFactory(*output, true);
    return result;
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9On12(
    UINT sdkVersion, D3D9ON12_ARGS* overrides, UINT overrideCount) {
    using Function = IDirect3D9* (WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);
    const bool initializedHere = novafix::runtime::EnsureInitialized();
    if (AddonsLoadedByCurrentCall(initializedHere)) {
        const ExternalDispatch dispatch = ResolveNewAddonDispatch(
            "Direct3DCreate9On12",
            reinterpret_cast<void*>(&Direct3DCreate9On12));
        if (dispatch.intercepted) {
            const auto outer = reinterpret_cast<Function>(dispatch.target);
            IDirect3D9* d3d = outer
                ? outer(sdkVersion, overrides, overrideCount) : nullptr;
            HookFactory(d3d, false);
            return d3d;
        }
    }
    const auto function = BackendExport<Function>("Direct3DCreate9On12");
    IDirect3D9* d3d = function ? function(sdkVersion, overrides, overrideCount) : nullptr;
    HookFactory(d3d, false);
    return d3d;
}

extern "C" HRESULT WINAPI Direct3DCreate9On12Ex(
    UINT sdkVersion, D3D9ON12_ARGS* overrides, UINT overrideCount,
    IDirect3D9Ex** output) {
    using Function = HRESULT (WINAPI*)(UINT, D3D9ON12_ARGS*, UINT, IDirect3D9Ex**);
    const bool initializedHere = novafix::runtime::EnsureInitialized();
    if (AddonsLoadedByCurrentCall(initializedHere)) {
        const ExternalDispatch dispatch = ResolveNewAddonDispatch(
            "Direct3DCreate9On12Ex",
            reinterpret_cast<void*>(&Direct3DCreate9On12Ex));
        if (dispatch.intercepted) {
            const auto outer = reinterpret_cast<Function>(dispatch.target);
            const HRESULT result = outer
                ? outer(sdkVersion, overrides, overrideCount, output)
                : D3DERR_NOTAVAILABLE;
            if (SUCCEEDED(result) && output) HookFactory(*output, true);
            return result;
        }
    }
    const auto function = BackendExport<Function>("Direct3DCreate9On12Ex");
    if (!function) return D3DERR_NOTAVAILABLE;
    const HRESULT result = function(sdkVersion, overrides, overrideCount, output);
    if (SUCCEEDED(result) && output) HookFactory(*output, true);
    return result;
}

extern "C" void* WINAPI Direct3DShaderValidatorCreate9() {
    using Function = void* (WINAPI*)();
    const auto function = BackendExport<Function>("Direct3DShaderValidatorCreate9");
    return function ? function() : nullptr;
}

extern "C" void WINAPI PSGPError(void* processVertices, UINT errorId, UINT value) {
    using Function = void (WINAPI*)(void*, UINT, UINT);
    const auto function = BackendExport<Function>("PSGPError");
    if (function) function(processVertices, errorId, value);
}

extern "C" void WINAPI PSGPSampleTexture(
    void* processVertices, UINT stage, float (*input)[4], UINT count,
    float (*output)[4]) {
    using Function = void (WINAPI*)(void*, UINT, float (*)[4], UINT, float (*)[4]);
    const auto function = BackendExport<Function>("PSGPSampleTexture");
    if (function) function(processVertices, stage, input, count, output);
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    using Function = int (WINAPI*)(D3DCOLOR, LPCWSTR);
    const auto function = BackendExport<Function>("D3DPERF_BeginEvent");
    return function ? function(color, name) : -1;
}

extern "C" int WINAPI D3DPERF_EndEvent() {
    using Function = int (WINAPI*)();
    const auto function = BackendExport<Function>("D3DPERF_EndEvent");
    return function ? function() : -1;
}

extern "C" DWORD WINAPI D3DPERF_GetStatus() {
    using Function = DWORD (WINAPI*)();
    const auto function = BackendExport<Function>("D3DPERF_GetStatus");
    return function ? function() : 0;
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    using Function = BOOL (WINAPI*)();
    const auto function = BackendExport<Function>("D3DPERF_QueryRepeatFrame");
    return function ? function() : FALSE;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    using Function = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    const auto function = BackendExport<Function>("D3DPERF_SetMarker");
    if (function) function(color, name);
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD options) {
    using Function = void (WINAPI*)(DWORD);
    const auto function = BackendExport<Function>("D3DPERF_SetOptions");
    if (function) function(options);
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    using Function = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    const auto function = BackendExport<Function>("D3DPERF_SetRegion");
    if (function) function(color, name);
}

extern "C" int WINAPI DebugSetLevel() {
    using Function = int (WINAPI*)();
    const auto function = BackendExport<Function>("DebugSetLevel");
    return function ? function() : 0;
}

extern "C" void WINAPI DebugSetMute() {
    using Function = void (WINAPI*)();
    const auto function = BackendExport<Function>("DebugSetMute");
    if (function) function();
}

extern "C" int WINAPI Direct3D9EnableMaximizedWindowedModeShim(UINT enabled) {
    using Function = int (WINAPI*)(UINT);
    const auto function = BackendExport<Function>("Direct3D9EnableMaximizedWindowedModeShim");
    return function ? function(enabled) : 0;
}

// Keep DXVK's annotation exports ordinal-only, matching its d3d9.def
extern "C" void WINAPI DXVK_RegisterAnnotation(void* annotation) {
    using Function = void (WINAPI*)(void*);
    const auto function = BackendOrdinal<Function>(28257);
    if (function) function(annotation);
}

extern "C" void WINAPI DXVK_UnRegisterAnnotation(void* annotation) {
    using Function = void (WINAPI*)(void*);
    const auto function = BackendOrdinal<Function>(28258);
    if (function) function(annotation);
}

extern "C" BOOL WINAPI NovaFix_IsOverlayOpen() {
    return novafix::overlay::IsOpen() ? TRUE : FALSE;
}

extern "C" void CALLBACK NovaFix_RestartHelper(HWND, HINSTANCE, LPSTR, int) {
    novafix::restart::RunHelper();
}
