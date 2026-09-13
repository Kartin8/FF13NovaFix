#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace novafix::compat::addon_storage {

enum class Import : std::size_t {
    CreateFileA,
    CreateFileW,
    GetPrivateProfileIntA,
    GetPrivateProfileIntW,
    GetPrivateProfileStringA,
    GetPrivateProfileStringW,
    GetPrivateProfileSectionA,
    GetPrivateProfileSectionW,
    GetPrivateProfileSectionNamesA,
    GetPrivateProfileSectionNamesW,
    GetPrivateProfileStructA,
    GetPrivateProfileStructW,
    WritePrivateProfileStringA,
    WritePrivateProfileStringW,
    WritePrivateProfileSectionA,
    WritePrivateProfileSectionW,
    WritePrivateProfileStructA,
    WritePrivateProfileStructW,
    Count,
};

// Routing is limited to each add-on's import table; no process-wide hooks
void Configure(std::wstring_view gameDirectory);
void Register(HMODULE module, std::wstring_view addonPath);

bool HookImport(HMODULE module, std::string_view ownerName,
                const char* functionName, void* replacement,
                Import import);
void* OriginalForCall(Import import, const void* returnAddress);

std::wstring RedirectPath(std::wstring_view requested,
                          const void* returnAddress);

} // namespace novafix::compat::addon_storage
