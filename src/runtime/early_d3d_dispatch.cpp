#include "runtime/early_d3d_dispatch.h"

#include "patch/iat_hook.h"
#include "diagnostics/log.h"
#include "common/module_path.h"
#include "compat/addons/addon_loader.h"
#include "patch/memory_access.h"
#include "runtime/bootstrap.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace novafix::runtime::early_d3d_dispatch {
namespace {

using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);

GetProcAddressFn g_originalGetProcAddress{};
thread_local bool g_forwardingToOuterLayer = false;

FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR name);

HMODULE ModuleForAddress(const void* address) {
    HMODULE module{};
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}

HMODULE OwnModule() {
    return ModuleForAddress(reinterpret_cast<const void*>(&OwnModule));
}

bool IsD3DFactoryExport(HMODULE module, LPCSTR name) {
    if (!module || module != OwnModule() ||
        reinterpret_cast<std::uintptr_t>(name) <= 0xFFFFu) {
        return false;
    }
    return std::strcmp(name, "Direct3DCreate9") == 0 ||
           std::strcmp(name, "Direct3DCreate9Ex") == 0 ||
           std::strcmp(name, "Direct3DCreate9On12") == 0 ||
           std::strcmp(name, "Direct3DCreate9On12Ex") == 0;
}

bool IsExternalResolver(void* candidate) {
    if (!candidate ||
        candidate == reinterpret_cast<void*>(&GetProcAddressHook) ||
        !patch::memory::IsExecutable(candidate, 1u)) {
        return false;
    }
    const HMODULE owner = ModuleForAddress(candidate);
    HMODULE own = OwnModule();
    return owner && owner != own;
}

FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR name) {
    GetProcAddressFn original = g_originalGetProcAddress;
    if (!original) return nullptr;
    if (g_forwardingToOuterLayer || !IsD3DFactoryExport(module, name)) {
        return original(module, name);
    }

    const bool initializedHere = runtime::EnsureInitialized();
    if (!initializedHere ||
        compat::addon_loader::LoadedCount() == 0u) {
        return original(module, name);
    }

    void* outer = ReadExecutableImportTarget(
        "kernel32.dll", "GetProcAddress");
    if (!IsExternalResolver(outer)) return original(module, name);

    g_forwardingToOuterLayer = true;
    FARPROC result = reinterpret_cast<GetProcAddressFn>(outer)(module, name);
    g_forwardingToOuterLayer = false;

    const std::wstring owner = path::ModuleFile(ModuleForAddress(outer));
    Log("D3D factory call forwarded to add-on: export=%s owner=%ls result=%p",
        name, owner.empty() ? L"<unknown>" : owner.c_str(), result);
    return result;
}

} // namespace

void Install() {
    void* original{};
    if (PatchExecutableImport(
            "kernel32.dll", "GetProcAddress",
            reinterpret_cast<void*>(&GetProcAddressHook), &original)) {
        g_originalGetProcAddress = reinterpret_cast<GetProcAddressFn>(original);
    }
}

} // namespace novafix::runtime::early_d3d_dispatch
