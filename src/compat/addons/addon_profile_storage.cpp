#include "compat/addons/addon_profile_storage.h"

#include "compat/addons/addon_storage.h"

#include <windows.h>
#include <intrin.h>

#include <string>
#include <string_view>

namespace novafix::compat::addon_profile_storage {
namespace {

using GetIntAFn = UINT (WINAPI*)(LPCSTR, LPCSTR, INT, LPCSTR);
using GetIntWFn = UINT (WINAPI*)(LPCWSTR, LPCWSTR, INT, LPCWSTR);
using GetStringAFn = DWORD (WINAPI*)(
    LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
using GetStringWFn = DWORD (WINAPI*)(
    LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD, LPCWSTR);
using GetSectionAFn = DWORD (WINAPI*)(LPCSTR, LPSTR, DWORD, LPCSTR);
using GetSectionWFn = DWORD (WINAPI*)(LPCWSTR, LPWSTR, DWORD, LPCWSTR);
using GetSectionNamesAFn = DWORD (WINAPI*)(LPSTR, DWORD, LPCSTR);
using GetSectionNamesWFn = DWORD (WINAPI*)(LPWSTR, DWORD, LPCWSTR);
using GetStructAFn = BOOL (WINAPI*)(LPCSTR, LPCSTR, LPVOID, UINT, LPCSTR);
using GetStructWFn = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, LPVOID, UINT, LPCWSTR);
using WriteStringAFn = BOOL (WINAPI*)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
using WriteStringWFn = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
using WriteSectionAFn = BOOL (WINAPI*)(LPCSTR, LPCSTR, LPCSTR);
using WriteSectionWFn = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR);
using WriteStructAFn = BOOL (WINAPI*)(LPCSTR, LPCSTR, LPVOID, UINT, LPCSTR);
using WriteStructWFn = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, LPVOID, UINT, LPCWSTR);

template <typename Function>
Function Original(addon_storage::Import import, const void* returnAddress) {
    return reinterpret_cast<Function>(
        addon_storage::OriginalForCall(import, returnAddress));
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

LPCWSTR ProfilePath(LPCWSTR requested, const void* returnAddress,
                    std::wstring& redirected) {
    if (!requested) return nullptr;
    redirected = addon_storage::RedirectPath(requested, returnAddress);
    return redirected.empty() ? requested : redirected.c_str();
}

LPCSTR ProfilePath(LPCSTR requested, const void* returnAddress,
                   std::wstring& redirectedWide,
                   std::string& redirectedAnsi) {
    if (!requested) return nullptr;
    redirectedWide = addon_storage::RedirectPath(Wide(requested), returnAddress);
    redirectedAnsi = Narrow(redirectedWide);
    return redirectedAnsi.empty() ? requested : redirectedAnsi.c_str();
}

UINT WINAPI GetIntWHook(
    LPCWSTR app, LPCWSTR key, INT defaultValue, LPCWSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetIntWFn original = Original<GetIntWFn>(
        addon_storage::Import::GetPrivateProfileIntW, caller);
    if (!original) return static_cast<UINT>(defaultValue);
    std::wstring path;
    return original(app, key, defaultValue, ProfilePath(fileName, caller, path));
}

UINT WINAPI GetIntAHook(
    LPCSTR app, LPCSTR key, INT defaultValue, LPCSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetIntAFn original = Original<GetIntAFn>(
        addon_storage::Import::GetPrivateProfileIntA, caller);
    if (!original) return static_cast<UINT>(defaultValue);
    std::wstring wide;
    std::string path;
    return original(app, key, defaultValue,
                    ProfilePath(fileName, caller, wide, path));
}

DWORD WINAPI GetStringWHook(
    LPCWSTR app, LPCWSTR key, LPCWSTR defaultValue, LPWSTR output,
    DWORD outputSize, LPCWSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetStringWFn original = Original<GetStringWFn>(
        addon_storage::Import::GetPrivateProfileStringW, caller);
    if (!original) return 0u;
    std::wstring path;
    return original(app, key, defaultValue, output, outputSize,
                    ProfilePath(fileName, caller, path));
}

DWORD WINAPI GetStringAHook(
    LPCSTR app, LPCSTR key, LPCSTR defaultValue, LPSTR output,
    DWORD outputSize, LPCSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetStringAFn original = Original<GetStringAFn>(
        addon_storage::Import::GetPrivateProfileStringA, caller);
    if (!original) return 0u;
    std::wstring wide;
    std::string path;
    return original(app, key, defaultValue, output, outputSize,
                    ProfilePath(fileName, caller, wide, path));
}

DWORD WINAPI GetSectionWHook(
    LPCWSTR app, LPWSTR output, DWORD outputSize, LPCWSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetSectionWFn original = Original<GetSectionWFn>(
        addon_storage::Import::GetPrivateProfileSectionW, caller);
    if (!original) return 0u;
    std::wstring path;
    return original(app, output, outputSize, ProfilePath(fileName, caller, path));
}

DWORD WINAPI GetSectionAHook(
    LPCSTR app, LPSTR output, DWORD outputSize, LPCSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetSectionAFn original = Original<GetSectionAFn>(
        addon_storage::Import::GetPrivateProfileSectionA, caller);
    if (!original) return 0u;
    std::wstring wide;
    std::string path;
    return original(app, output, outputSize,
                    ProfilePath(fileName, caller, wide, path));
}

DWORD WINAPI GetSectionNamesWHook(
    LPWSTR output, DWORD outputSize, LPCWSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetSectionNamesWFn original = Original<GetSectionNamesWFn>(
        addon_storage::Import::GetPrivateProfileSectionNamesW, caller);
    if (!original) return 0u;
    std::wstring path;
    return original(output, outputSize, ProfilePath(fileName, caller, path));
}

DWORD WINAPI GetSectionNamesAHook(
    LPSTR output, DWORD outputSize, LPCSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetSectionNamesAFn original = Original<GetSectionNamesAFn>(
        addon_storage::Import::GetPrivateProfileSectionNamesA, caller);
    if (!original) return 0u;
    std::wstring wide;
    std::string path;
    return original(output, outputSize,
                    ProfilePath(fileName, caller, wide, path));
}

BOOL WINAPI GetStructWHook(
    LPCWSTR section, LPCWSTR key, LPVOID data, UINT size, LPCWSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetStructWFn original = Original<GetStructWFn>(
        addon_storage::Import::GetPrivateProfileStructW, caller);
    if (!original) return FALSE;
    std::wstring path;
    return original(section, key, data, size, ProfilePath(fileName, caller, path));
}

BOOL WINAPI GetStructAHook(
    LPCSTR section, LPCSTR key, LPVOID data, UINT size, LPCSTR fileName) {
    const void* caller = _ReturnAddress();
    const GetStructAFn original = Original<GetStructAFn>(
        addon_storage::Import::GetPrivateProfileStructA, caller);
    if (!original) return FALSE;
    std::wstring wide;
    std::string path;
    return original(section, key, data, size,
                    ProfilePath(fileName, caller, wide, path));
}

BOOL WINAPI WriteStringWHook(
    LPCWSTR app, LPCWSTR key, LPCWSTR value, LPCWSTR fileName) {
    const void* caller = _ReturnAddress();
    const WriteStringWFn original = Original<WriteStringWFn>(
        addon_storage::Import::WritePrivateProfileStringW, caller);
    if (!original) return FALSE;
    std::wstring path;
    return original(app, key, value, ProfilePath(fileName, caller, path));
}

BOOL WINAPI WriteStringAHook(
    LPCSTR app, LPCSTR key, LPCSTR value, LPCSTR fileName) {
    const void* caller = _ReturnAddress();
    const WriteStringAFn original = Original<WriteStringAFn>(
        addon_storage::Import::WritePrivateProfileStringA, caller);
    if (!original) return FALSE;
    std::wstring wide;
    std::string path;
    return original(app, key, value,
                    ProfilePath(fileName, caller, wide, path));
}

BOOL WINAPI WriteSectionWHook(
    LPCWSTR app, LPCWSTR value, LPCWSTR fileName) {
    const void* caller = _ReturnAddress();
    const WriteSectionWFn original = Original<WriteSectionWFn>(
        addon_storage::Import::WritePrivateProfileSectionW, caller);
    if (!original) return FALSE;
    std::wstring path;
    return original(app, value, ProfilePath(fileName, caller, path));
}

BOOL WINAPI WriteSectionAHook(
    LPCSTR app, LPCSTR value, LPCSTR fileName) {
    const void* caller = _ReturnAddress();
    const WriteSectionAFn original = Original<WriteSectionAFn>(
        addon_storage::Import::WritePrivateProfileSectionA, caller);
    if (!original) return FALSE;
    std::wstring wide;
    std::string path;
    return original(app, value, ProfilePath(fileName, caller, wide, path));
}

BOOL WINAPI WriteStructWHook(
    LPCWSTR section, LPCWSTR key, LPVOID data, UINT size, LPCWSTR fileName) {
    const void* caller = _ReturnAddress();
    const WriteStructWFn original = Original<WriteStructWFn>(
        addon_storage::Import::WritePrivateProfileStructW, caller);
    if (!original) return FALSE;
    std::wstring path;
    return original(section, key, data, size, ProfilePath(fileName, caller, path));
}

BOOL WINAPI WriteStructAHook(
    LPCSTR section, LPCSTR key, LPVOID data, UINT size, LPCSTR fileName) {
    const void* caller = _ReturnAddress();
    const WriteStructAFn original = Original<WriteStructAFn>(
        addon_storage::Import::WritePrivateProfileStructA, caller);
    if (!original) return FALSE;
    std::wstring wide;
    std::string path;
    return original(section, key, data, size,
                    ProfilePath(fileName, caller, wide, path));
}

} // namespace

unsigned Install(HMODULE module, std::string_view ownerName) {
    unsigned installed{};
    const auto add = [&](const char* name, auto hook,
                         addon_storage::Import import) {
        installed += addon_storage::HookImport(
            module, ownerName, name, reinterpret_cast<void*>(hook), import)
            ? 1u : 0u;
    };
    add("GetPrivateProfileIntA", &GetIntAHook,
        addon_storage::Import::GetPrivateProfileIntA);
    add("GetPrivateProfileIntW", &GetIntWHook,
        addon_storage::Import::GetPrivateProfileIntW);
    add("GetPrivateProfileStringA", &GetStringAHook,
        addon_storage::Import::GetPrivateProfileStringA);
    add("GetPrivateProfileStringW", &GetStringWHook,
        addon_storage::Import::GetPrivateProfileStringW);
    add("GetPrivateProfileSectionA", &GetSectionAHook,
        addon_storage::Import::GetPrivateProfileSectionA);
    add("GetPrivateProfileSectionW", &GetSectionWHook,
        addon_storage::Import::GetPrivateProfileSectionW);
    add("GetPrivateProfileSectionNamesA", &GetSectionNamesAHook,
        addon_storage::Import::GetPrivateProfileSectionNamesA);
    add("GetPrivateProfileSectionNamesW", &GetSectionNamesWHook,
        addon_storage::Import::GetPrivateProfileSectionNamesW);
    add("GetPrivateProfileStructA", &GetStructAHook,
        addon_storage::Import::GetPrivateProfileStructA);
    add("GetPrivateProfileStructW", &GetStructWHook,
        addon_storage::Import::GetPrivateProfileStructW);
    add("WritePrivateProfileStringA", &WriteStringAHook,
        addon_storage::Import::WritePrivateProfileStringA);
    add("WritePrivateProfileStringW", &WriteStringWHook,
        addon_storage::Import::WritePrivateProfileStringW);
    add("WritePrivateProfileSectionA", &WriteSectionAHook,
        addon_storage::Import::WritePrivateProfileSectionA);
    add("WritePrivateProfileSectionW", &WriteSectionWHook,
        addon_storage::Import::WritePrivateProfileSectionW);
    add("WritePrivateProfileStructA", &WriteStructAHook,
        addon_storage::Import::WritePrivateProfileStructA);
    add("WritePrivateProfileStructW", &WriteStructWHook,
        addon_storage::Import::WritePrivateProfileStructW);
    return installed;
}

} // namespace novafix::compat::addon_profile_storage
