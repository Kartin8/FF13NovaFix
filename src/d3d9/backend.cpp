#include "d3d9/backend.h"

#include "diagnostics/log.h"
#include "common/module_path.h"
#include "compat/compatibility.h"
#include "compat/platform.h"
#include "settings/performance_settings.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace novafix::d3d9::backend {
namespace {

enum class Kind {
    Unavailable,
    System,
    Chain,
    DXVK,
    ReShade,
    Explicit,
};

struct Candidate {
    std::wstring path;
    Kind kind{Kind::Unavailable};
};

struct FileIdentity {
    DWORD volumeSerial{};
    DWORD indexHigh{};
    DWORD indexLow{};
    bool valid{};
};

INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_module = nullptr;
Kind g_kind = Kind::Unavailable;
std::wstring g_path;

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Unavailable: return "unavailable";
    case Kind::System: return "system";
    case Kind::Chain: return "chain";
    case Kind::DXVK: return "dxvk";
    case Kind::ReShade: return "reshade";
    case Kind::Explicit: return "explicit";
    }
    return "unknown";
}

HMODULE OwnModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&g_module), &module);
    return module;
}

std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (!written || written >= required) return {};
    value.resize(written);
    return value;
}

bool IsAbsolute(std::wstring_view value) {
    if (!value.empty() && (value[0] == L'\\' || value[0] == L'/')) return true;
    return value.size() >= 3 && ((value[0] >= L'A' && value[0] <= L'Z') ||
                                 (value[0] >= L'a' && value[0] <= L'z')) &&
           value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}

std::wstring BesideProxy(std::wstring_view fileName) {
    const std::wstring directory = path::Directory(path::ModuleFile(OwnModule()));
    if (directory.empty()) return {};
    return directory + L"\\" + std::wstring(fileName);
}

std::vector<Candidate> BuildCandidates() {
    const std::wstring configured = EnvironmentValue(L"NOVAFIX_D3D9_BACKEND");
    if (!configured.empty()) {
        if (_wcsicmp(configured.c_str(), L"system") == 0) {
            return {{path::SystemFile(L"d3d9.dll"), Kind::System}};
        }
        return {{IsAbsolute(configured) ? configured : BesideProxy(configured), Kind::Explicit},
                {path::SystemFile(L"d3d9.dll"), Kind::System}};
    }

    const settings::D3D9Backend preference = settings::ActivePerformance().d3d9Backend;
    switch (preference) {
    case settings::D3D9Backend::System:
        return {{path::SystemFile(L"d3d9.dll"), Kind::System}};
    case settings::D3D9Backend::LocalDXVK:
        return {
            {BesideProxy(L"dxvk.dll"), Kind::DXVK},
            {path::SystemFile(L"d3d9.dll"), Kind::System},
        };
    case settings::D3D9Backend::Automatic:
        return {
            {BesideProxy(L"d3d9_chain.dll"), Kind::Chain},
            {BesideProxy(L"ReShade\\d3d9.dll"), Kind::ReShade},
            {path::SystemFile(L"d3d9.dll"), Kind::System},
        };
    }
    return {{path::SystemFile(L"d3d9.dll"), Kind::System}};
}

FileIdentity IdentityOf(const std::wstring& filePath) {
    if (filePath.empty()) return {};
    HANDLE file = CreateFileW(
        filePath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    BY_HANDLE_FILE_INFORMATION information{};
    const bool queried = GetFileInformationByHandle(file, &information) != FALSE;
    CloseHandle(file);
    if (!queried) return {};
    return {
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow,
        true,
    };
}

bool SameFile(const std::wstring& left, const std::wstring& right) {
    const FileIdentity leftIdentity = IdentityOf(left);
    const FileIdentity rightIdentity = IdentityOf(right);
    if (leftIdentity.valid && rightIdentity.valid) {
        return leftIdentity.volumeSerial == rightIdentity.volumeSerial &&
               leftIdentity.indexHigh == rightIdentity.indexHigh &&
               leftIdentity.indexLow == rightIdentity.indexLow;
    }
    return !left.empty() && !right.empty() && path::Equivalent(left, right);
}

bool IsProxyPath(const std::wstring& candidatePath) {
    return SameFile(path::ModuleFile(OwnModule()), candidatePath);
}

HMODULE LoadBackendLibrary(const std::wstring& filePath, DWORD* error) {
    if (error) *error = ERROR_SUCCESS;
    constexpr DWORD safeFlags =
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    HMODULE module = LoadLibraryExW(filePath.c_str(), nullptr, safeFlags);
    if (module) return module;

    const DWORD safeError = GetLastError();
    // Older loader implementations may reject the search flags. The altered
    // search path retry keeps the backend directory first while still allowing
    // chained renderers to resolve legacy PATH dependencies.
    module = LoadLibraryExW(
        filePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module && error) {
        const DWORD fallbackError = GetLastError();
        *error = fallbackError ? fallbackError : safeError;
    }
    return module;
}

bool HasDxvkAnnotationBridge(HMODULE module) {
    return module &&
           GetProcAddress(module, MAKEINTRESOURCEA(28257)) &&
           GetProcAddress(module, MAKEINTRESOURCEA(28258));
}

Kind ClassifyLoaded(HMODULE module, const std::wstring& loadedPath,
                    Kind requestedKind) {
    if (HasDxvkAnnotationBridge(module)) return Kind::DXVK;
    if (module && GetProcAddress(module, "ReShadeVersion")) {
        return Kind::ReShade;
    }
    const std::wstring systemD3D9 = path::SystemFile(L"d3d9.dll");
    if (!loadedPath.empty() && SameFile(loadedPath, systemD3D9)) {
        return Kind::System;
    }
    return requestedKind;
}

bool IsProxyItself(HMODULE loaded, const std::wstring& loadedPath) {
    const HMODULE ownModule = OwnModule();
    if (loaded == ownModule) return true;
    const std::wstring ownPath = path::ModuleFile(ownModule);
    return SameFile(ownPath, loadedPath);
}

BOOL CALLBACK Load(PINIT_ONCE, PVOID, PVOID*) {
    bool requestedDxvkFailed = false;
    for (const Candidate& candidate : BuildCandidates()) {
        if (candidate.path.empty()) continue;
        if (candidate.kind != Kind::System &&
            GetFileAttributesW(candidate.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (candidate.kind == Kind::DXVK) {
                requestedDxvkFailed = true;
                LogWarning("Local DXVK was selected but dxvk.dll is missing: %ls",
                    candidate.path.c_str());
            }
            continue;
        }
        if (IsProxyPath(candidate.path)) {
            LogError("Recursive D3D9 proxy candidate rejected before load: %ls",
                candidate.path.c_str());
            continue;
        }
        if (candidate.kind != Kind::System &&
            compat::compatibility::ShouldBlockBackend(candidate.path)) {
            if (candidate.kind == Kind::DXVK) requestedDxvkFailed = true;
            continue;
        }

        DWORD loadError = ERROR_SUCCESS;
        HMODULE loaded = LoadBackendLibrary(candidate.path, &loadError);
        if (!loaded) {
            if (candidate.kind == Kind::DXVK) requestedDxvkFailed = true;
            Log("D3D9 backend candidate failed: %ls error=%lu",
                candidate.path.c_str(), loadError);
            continue;
        }

        const std::wstring loadedPath = path::ModuleFile(loaded);
        if (IsProxyItself(loaded, loadedPath)) {
            LogError("Recursive D3D9 proxy candidate rejected: %ls", candidate.path.c_str());
            FreeLibrary(loaded);
            continue;
        }
        if (!GetProcAddress(loaded, "Direct3DCreate9")) {
            Log("D3D9 backend candidate has no Direct3DCreate9 export: %ls", candidate.path.c_str());
            FreeLibrary(loaded);
            continue;
        }

        g_module = loaded;
        g_path = loadedPath.empty() ? candidate.path : loadedPath;
        g_kind = ClassifyLoaded(loaded, g_path, candidate.kind);
        LogInfo("D3D9 renderer: %s (%ls)", KindName(g_kind), g_path.c_str());
        if (requestedDxvkFailed && g_kind == Kind::System) {
            LogWarning("Could not load local DXVK: using system Direct3D 9");
        }
        return TRUE;
    }

    LogError("No usable D3D9 backend was found");
    return TRUE;
}

void EnsureLoaded() {
    InitOnceExecuteOnce(&g_loadOnce, &Load, nullptr, nullptr);
}

} // namespace

HMODULE Module() {
    EnsureLoaded();
    return g_module;
}

FARPROC Export(const char* name) {
    HMODULE module = Module();
    return module && name ? GetProcAddress(module, name) : nullptr;
}

FARPROC ExportOrdinal(WORD ordinal) {
    HMODULE module = Module();
    return module && ordinal ? GetProcAddress(module, MAKEINTRESOURCEA(ordinal)) : nullptr;
}

} // namespace novafix::d3d9::backend
