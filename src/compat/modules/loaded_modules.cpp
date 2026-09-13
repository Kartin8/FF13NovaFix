#include "compat/modules/loaded_modules.h"

#include "diagnostics/log.h"
#include "common/module_path.h"
#include "compat/addons/addon_directories.h"

#include <windows.h>
#include <tlhelp32.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace novafix::compat::loaded_modules {
namespace {

INIT_ONCE g_scanOnce = INIT_ONCE_STATIC_INIT;
Report g_report;

HMODULE OwnModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&g_scanOnce), &module);
    return module;
}

std::wstring Join(std::wstring_view left, std::wstring_view right) {
    if (left.empty()) return std::wstring(right);
    std::wstring result(left);
    if (result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
    result.append(right);
    return result;
}

bool IsWithinDirectory(std::wstring_view filePath, std::wstring_view directory) {
    while (!directory.empty() &&
           (directory.back() == L'\\' || directory.back() == L'/')) {
        directory.remove_suffix(1);
    }
    if (directory.empty() || filePath.size() <= directory.size()) return false;
    const std::wstring prefix(filePath.substr(0, directory.size()));
    const std::wstring root(directory);
    if (_wcsicmp(prefix.c_str(), root.c_str()) != 0) return false;
    const wchar_t separator = filePath[directory.size()];
    return separator == L'\\' || separator == L'/';
}

bool HasModuleExtension(std::wstring_view filePath) {
    const size_t dot = filePath.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    const std::wstring extension(filePath.substr(dot));
    return _wcsicmp(extension.c_str(), L".dll") == 0 ||
           _wcsicmp(extension.c_str(), L".asi") == 0;
}

std::wstring FileStem(std::wstring_view filePath) {
    const size_t separator = filePath.find_last_of(L"\\/");
    const size_t begin = separator == std::wstring_view::npos ? 0 : separator + 1;
    size_t end = filePath.find_last_of(L'.');
    if (end == std::wstring_view::npos || end < begin) end = filePath.size();
    return std::wstring(filePath.substr(begin, end - begin));
}

std::wstring FileName(std::wstring_view filePath) {
    const size_t separator = filePath.find_last_of(L"\\/");
    return std::wstring(filePath.substr(
        separator == std::wstring_view::npos ? 0 : separator + 1));
}

bool IsStockGameModule(std::wstring_view modulePath,
                       const std::wstring& novaDirectory) {
    // Hide stock middleware beside the game, but not inside add-on directories
    if (!path::Equivalent(path::Directory(modulePath), novaDirectory)) return false;
    const std::wstring name = FileName(modulePath);
    constexpr std::array<const wchar_t*, 3> stockNames{
        L"bink2w32.dll",
        L"steam_api.dll",
        L"sgw.dll",
    };
    return std::any_of(stockNames.begin(), stockNames.end(),
                       [&](const wchar_t* stockName) {
                           return _wcsicmp(name.c_str(), stockName) == 0;
                       });
}

struct Translation {
    WORD language{};
    WORD codePage{};
};

std::wstring QueryVersionString(const std::vector<std::byte>& data,
                                WORD language, WORD codePage,
                                const wchar_t* field) {
    wchar_t key[96]{};
    _snwprintf_s(key, std::size(key), _TRUNCATE,
                 L"\\StringFileInfo\\%04x%04x\\%ls",
                 language, codePage, field);
    void* value = nullptr;
    UINT characters = 0;
    if (!VerQueryValueW(data.data(), key, &value, &characters) ||
        !value || characters <= 1) {
        return {};
    }
    const auto* text = static_cast<const wchar_t*>(value);
    std::wstring result(text, characters - 1);
    while (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

std::wstring VersionDisplayName(const std::wstring& modulePath) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(modulePath.c_str(), &ignored);
    if (!size) return {};
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(modulePath.c_str(), 0, size, data.data())) return {};

    void* translationData = nullptr;
    UINT translationBytes = 0;
    std::vector<Translation> translations;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                       &translationData, &translationBytes) &&
        translationData && translationBytes >= sizeof(Translation)) {
        const auto* values = static_cast<const Translation*>(translationData);
        translations.assign(values, values + translationBytes / sizeof(Translation));
    }
    constexpr std::array<Translation, 2> fallbacks{{
        {0x0409, 0x04B0},
        {0x0409, 0x04E4},
    }};
    translations.insert(translations.end(), fallbacks.begin(), fallbacks.end());

    for (const wchar_t* field : {L"ProductName", L"FileDescription"}) {
        for (const Translation translation : translations) {
            std::wstring value = QueryVersionString(
                data, translation.language, translation.codePage, field);
            if (!value.empty()) return value;
        }
    }
    return {};
}

bool ContainsPath(const std::vector<Entry>& entries, const std::wstring& candidate) {
    return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) {
        return path::Equivalent(entry.path, candidate);
    });
}

BOOL CALLBACK Scan(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring ownPath = path::ModuleFile(OwnModule());
    const std::wstring novaDirectory = path::Directory(ownPath);
    if (ownPath.empty() || novaDirectory.empty()) {
        Log("Loaded-module notification disabled: NovaFix path unavailable");
        return TRUE;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        LogWarning("Loaded-module notification scan failed: error=%lu", GetLastError());
        return TRUE;
    }

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module)) {
        do {
            const std::wstring modulePath(module.szExePath);
            if (!IsInNotificationScope(modulePath, novaDirectory, ownPath) ||
                ContainsPath(g_report.entries, modulePath)) {
                continue;
            }
            g_report.entries.push_back({modulePath, DisplayName(modulePath)});
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);

    std::sort(g_report.entries.begin(), g_report.entries.end(),
              [](const Entry& left, const Entry& right) {
                  return CompareStringOrdinal(
                             left.displayName.c_str(), -1,
                             right.displayName.c_str(), -1, TRUE) == CSTR_LESS_THAN;
              });
    for (const Entry& entry : g_report.entries) {
        Log("Loaded local module: name=%ls path=%ls",
            entry.displayName.c_str(), entry.path.c_str());
    }
    return TRUE;
}

} // namespace

bool IsInNotificationScope(const std::wstring& modulePath,
                           const std::wstring& novaDirectory,
                           const std::wstring& novaModulePath) {
    if (modulePath.empty() || novaDirectory.empty() ||
        path::Equivalent(modulePath, novaModulePath) ||
        !HasModuleExtension(modulePath)) {
        return false;
    }
    if (path::Equivalent(path::Directory(modulePath), novaDirectory)) {
        return !IsStockGameModule(modulePath, novaDirectory);
    }
    return std::any_of(
        compat::kAddonDirectoryNames.begin(),
        compat::kAddonDirectoryNames.end(),
        [&](std::wstring_view directoryName) {
            return IsWithinDirectory(
                modulePath, Join(novaDirectory, directoryName));
        });
}

std::wstring DisplayName(const std::wstring& modulePath) {
    std::wstring result = VersionDisplayName(modulePath);
    if (!result.empty()) return result;
    result = FileStem(modulePath);
    return result.empty() ? L"<unnamed add-on>" : result;
}

std::wstring NotificationMessage(const Report& report) {
    std::wstring message = L"Loaded add-ons:";
    for (const Entry& entry : report.entries) {
        message += L"\n- ";
        message += entry.displayName.empty()
            ? L"<unnamed add-on>" : entry.displayName;
    }
    return message;
}

const Report& CurrentReport() {
    InitOnceExecuteOnce(&g_scanOnce, &Scan, nullptr, nullptr);
    return g_report;
}

std::wstring Signature(const Report& report) {
    std::vector<std::wstring> identities;
    identities.reserve(report.entries.size());
    for (const Entry& entry : report.entries) {
        std::wstring identity = entry.path;
        std::transform(identity.begin(), identity.end(), identity.begin(),
                       [](wchar_t character) {
                           if (character == L'/') return L'\\';
                           return static_cast<wchar_t>(std::towlower(character));
                       });
        identities.push_back(std::move(identity));
    }
    std::sort(identities.begin(), identities.end());

    std::uint64_t hash = 14695981039346656037ull;
    const auto append = [&](wchar_t character) {
        const auto value = static_cast<std::uint32_t>(character);
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            hash ^= static_cast<std::uint8_t>(value >> shift);
            hash *= 1099511628211ull;
        }
    };
    for (const std::wstring& identity : identities) {
        for (const wchar_t character : identity) append(character);
        append(L'\0');
    }

    std::wostringstream text;
    text << std::hex << std::uppercase << std::setw(16) << std::setfill(L'0')
         << hash;
    return text.str();
}

} // namespace novafix::compat::loaded_modules
