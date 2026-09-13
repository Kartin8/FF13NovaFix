#include "compat/addons/addon_storage.h"
#include "compat/addons/addon_profile_storage.h"

#include "common/module_path.h"
#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "patch/iat_hook.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace novafix::compat::addon_storage {
namespace {

using CreateFileAFn = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD,
    LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD,
    LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

constexpr std::size_t kImportCount = static_cast<std::size_t>(Import::Count);

struct ModuleRoute {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::wstring directory;
    std::array<void*, kImportCount> originals{};
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<ModuleRoute> g_routes;
std::wstring g_gameDirectory;
std::array<void*, kImportCount> g_systemFunctions{};

constexpr std::array<const char*, kImportCount> kImportNames{
    "CreateFileA",
    "CreateFileW",
    "GetPrivateProfileIntA",
    "GetPrivateProfileIntW",
    "GetPrivateProfileStringA",
    "GetPrivateProfileStringW",
    "GetPrivateProfileSectionA",
    "GetPrivateProfileSectionW",
    "GetPrivateProfileSectionNamesA",
    "GetPrivateProfileSectionNamesW",
    "GetPrivateProfileStructA",
    "GetPrivateProfileStructW",
    "WritePrivateProfileStringA",
    "WritePrivateProfileStringW",
    "WritePrivateProfileSectionA",
    "WritePrivateProfileSectionW",
    "WritePrivateProfileStructA",
    "WritePrivateProfileStructW",
};

constexpr std::size_t Index(Import import) {
    return static_cast<std::size_t>(import);
}

template <typename Character>
constexpr Character LowerAscii(Character character) {
    return character >= static_cast<Character>('A') &&
                   character <= static_cast<Character>('Z')
        ? static_cast<Character>(character +
                                 (static_cast<Character>('a') -
                                  static_cast<Character>('A')))
        : character;
}

template <typename Character>
bool IniExtension(std::basic_string_view<Character> filePath) {
    if (filePath.size() < 4u ||
        filePath[filePath.size() - 4u] != static_cast<Character>('.')) {
        return false;
    }
    const Character first = LowerAscii(filePath[filePath.size() - 3u]);
    const Character second = LowerAscii(filePath[filePath.size() - 2u]);
    const Character third = LowerAscii(filePath[filePath.size() - 1u]);
    return first == static_cast<Character>('i') &&
           second == static_cast<Character>('n') &&
           third == static_cast<Character>('i');
}

std::wstring FileName(std::wstring_view filePath) {
    const std::size_t separator = filePath.find_last_of(L"\\/");
    return std::wstring(filePath.substr(
        separator == std::wstring_view::npos ? 0u : separator + 1u));
}

bool SameFileName(std::wstring_view left, const wchar_t* right) {
    if (!right) return false;
    const std::wstring leftText(left);
    return _wcsicmp(leftText.c_str(), right) == 0;
}

bool ReservedHostIni(std::wstring_view fileName) {
    return SameFileName(fileName, L"NovaFix.ini");
}

std::wstring FullPath(std::wstring_view filePath) {
    if (filePath.empty()) return {};
    const std::wstring source(filePath);
    const DWORD required = GetFullPathNameW(source.c_str(), 0u, nullptr, nullptr);
    if (!required) return {};
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required));
    const DWORD written = GetFullPathNameW(
        source.c_str(), required, buffer.data(), nullptr);
    if (!written || written >= required) return {};
    return std::wstring(buffer.data(), written);
}

std::size_t ModuleImageSize(HMODULE module) {
    if (!module) return 0u;
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0u;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE
        ? nt->OptionalHeader.SizeOfImage : 0u;
}

ModuleRoute* FindRouteLocked(std::uintptr_t address) {
    const auto route = std::find_if(
        g_routes.begin(), g_routes.end(), [&](const ModuleRoute& item) {
            return address >= item.begin && address < item.end;
        });
    return route == g_routes.end() ? nullptr : &*route;
}

const ModuleRoute* FindRouteInFramesLocked(
    const std::array<void*, 24>& frames, USHORT frameCount) {
    for (USHORT index = 0u; index < frameCount; ++index) {
        if (ModuleRoute* route = FindRouteLocked(
                reinterpret_cast<std::uintptr_t>(frames[index]))) {
            return route;
        }
    }
    return nullptr;
}

std::wstring DirectoryForCall(const void* returnAddress) {
    const std::uintptr_t directAddress =
        reinterpret_cast<std::uintptr_t>(returnAddress);
    AcquireSRWLockShared(&g_lock);
    const ModuleRoute* direct = FindRouteLocked(directAddress);
    std::wstring directory = direct ? direct->directory : std::wstring{};
    ReleaseSRWLockShared(&g_lock);
    if (!directory.empty()) return directory;

    std::array<void*, 24> frames{};
    const USHORT frameCount = RtlCaptureStackBackTrace(
        1u, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    AcquireSRWLockShared(&g_lock);
    const ModuleRoute* route = FindRouteInFramesLocked(frames, frameCount);
    directory = route ? route->directory : std::wstring{};
    ReleaseSRWLockShared(&g_lock);
    return directory;
}

void StoreOriginal(HMODULE module, Import import, void* original) {
    if (!module || !original) return;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(module);
    AcquireSRWLockExclusive(&g_lock);
    ModuleRoute* route = FindRouteLocked(address);
    if (route) route->originals[Index(import)] = original;
    ReleaseSRWLockExclusive(&g_lock);
}

std::wstring RedirectedPath(std::wstring_view requested,
                            const void* returnAddress) {
    if (!IniExtension(requested)) return {};
    const std::wstring source = FullPath(requested);
    if (source.empty() || g_gameDirectory.empty() ||
        !path::Equivalent(path::Directory(source), g_gameDirectory)) {
        return {};
    }

    const std::wstring fileName = FileName(source);
    if (ReservedHostIni(fileName)) return {};

    std::wstring directory = DirectoryForCall(returnAddress);
    if (directory.empty() || path::Equivalent(directory, g_gameDirectory)) {
        return {};
    }
    if (directory.back() != L'\\' && directory.back() != L'/') {
        directory.push_back(L'\\');
    }
    directory.append(fileName);
    return directory;
}

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_ACP, 0, text.data(), static_cast<int>(text.size()),
            result.data(), required) != required) {
        return {};
    }
    return result;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_ACP, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_ACP, 0, text.data(), static_cast<int>(text.size()),
            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

HANDLE WINAPI CreateFileWHook(
    LPCWSTR fileName, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags,
    HANDLE templateFile) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.addons.create-file-w-total");
    const auto original = reinterpret_cast<CreateFileWFn>(
        OriginalForCall(Import::CreateFileW, _ReturnAddress()));
    if (!original) return INVALID_HANDLE_VALUE;
    if (!fileName) {
        return original(fileName, access, share, security, creation, flags,
                        templateFile);
    }

    const std::wstring redirected = RedirectedPath(fileName, _ReturnAddress());
    return original(redirected.empty() ? fileName : redirected.c_str(),
                    access, share, security, creation, flags, templateFile);
}

HANDLE WINAPI CreateFileAHook(
    LPCSTR fileName, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags,
    HANDLE templateFile) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.addons.create-file-a-total");
    const auto original = reinterpret_cast<CreateFileAFn>(
        OriginalForCall(Import::CreateFileA, _ReturnAddress()));
    if (!original) return INVALID_HANDLE_VALUE;
    if (!fileName || !IniExtension(std::string_view(fileName))) {
        return original(fileName, access, share, security, creation, flags,
                        templateFile);
    }

    const std::wstring redirected = RedirectedPath(Wide(fileName), _ReturnAddress());
    const std::string redirectedAnsi = Narrow(redirected);
    return original(redirectedAnsi.empty() ? fileName : redirectedAnsi.c_str(),
                    access, share, security, creation, flags, templateFile);
}

std::string OwnerName(HMODULE module) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "addon-%p", static_cast<void*>(module));
    return text;
}

} // namespace

void Configure(std::wstring_view gameDirectory) {
    if (gameDirectory.empty() || !g_gameDirectory.empty()) return;
    g_gameDirectory.assign(gameDirectory);

    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel) {
        for (std::size_t index = 0u; index < kImportNames.size(); ++index) {
            g_systemFunctions[index] = reinterpret_cast<void*>(
                GetProcAddress(kernel, kImportNames[index]));
        }
    }
}

bool HookImport(HMODULE module, std::string_view ownerName,
                const char* functionName, void* replacement,
                Import import) {
    if (!module || ownerName.empty() || !functionName || !replacement ||
        import == Import::Count) {
        return false;
    }

    const std::string owner(ownerName);
    constexpr std::array<const char*, 2> kImportDlls{
        "kernel32.dll", "kernelbase.dll"};
    for (const char* importDll : kImportDlls) {
        void* original{};
        if (PatchModuleImport(module, owner.c_str(), importDll, functionName,
                              replacement, &original)) {
            StoreOriginal(module, import, original);
            return true;
        }
    }
    return false;
}

void* OriginalForCall(Import import, const void* returnAddress) {
    if (import == Import::Count) return nullptr;
    const std::size_t index = Index(import);
    const std::uintptr_t directAddress =
        reinterpret_cast<std::uintptr_t>(returnAddress);
    AcquireSRWLockShared(&g_lock);
    const ModuleRoute* direct = FindRouteLocked(directAddress);
    void* original = direct ? direct->originals[index] : nullptr;
    ReleaseSRWLockShared(&g_lock);
    if (original) return original;

    std::array<void*, 24> frames{};
    const USHORT frameCount = RtlCaptureStackBackTrace(
        1u, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    AcquireSRWLockShared(&g_lock);
    const ModuleRoute* route = FindRouteInFramesLocked(frames, frameCount);
    original = route ? route->originals[index] : nullptr;
    ReleaseSRWLockShared(&g_lock);
    return original ? original : g_systemFunctions[index];
}

std::wstring RedirectPath(std::wstring_view requested,
                          const void* returnAddress) {
    return RedirectedPath(requested, returnAddress);
}

void Register(HMODULE module, std::wstring_view addonPath) {
    const std::size_t size = ModuleImageSize(module);
    const std::wstring addonPathText(addonPath);
    const std::wstring directory = path::Directory(addonPathText);
    if (!module || !size || directory.empty()) return;

    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(module);
    bool inserted = false;
    AcquireSRWLockExclusive(&g_lock);
    if (!FindRouteLocked(begin)) {
        ModuleRoute route{};
        route.begin = begin;
        route.end = begin + size;
        route.directory = directory;
        route.originals = g_systemFunctions;
        g_routes.push_back(std::move(route));
        inserted = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!inserted) return;

    const std::string owner = OwnerName(module);
    unsigned installed{};
    installed += HookImport(
        module, owner, "CreateFileA", reinterpret_cast<void*>(&CreateFileAHook),
        Import::CreateFileA) ? 1u : 0u;
    installed += HookImport(
        module, owner, "CreateFileW", reinterpret_cast<void*>(&CreateFileWHook),
        Import::CreateFileW) ? 1u : 0u;
    installed += addon_profile_storage::Install(module, owner);

    if (installed) {
        Log("Add-on INI storage: path=%ls hooks=%u",
            addonPathText.c_str(), installed);
    }
}

} // namespace novafix::compat::addon_storage
