#include "d3d9/reshade_chain.h"

#include "diagnostics/log.h"
#include "settings/ini_value.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace novafix::d3d9::reshade_chain {
namespace {

std::wstring ReadIni(const std::wstring& file, const wchar_t* section,
                     const wchar_t* key) {
    std::vector<wchar_t> value(32768);
    const DWORD length = GetPrivateProfileStringW(
        section, key, L"", value.data(), static_cast<DWORD>(value.size()),
        file.c_str());
    return length < value.size() - 1 ? std::wstring(value.data(), length)
                                     : std::wstring{};
}

std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required) return {};
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), required);
    if (!length || length >= required) return {};
    value.resize(length);
    return value;
}

std::filesystem::path ExistingDirectory(
    const std::wstring& value, const std::filesystem::path& relativeTo) {
    if (value.empty()) return {};
    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!required) return {};
    std::wstring expanded(required, L'\0');
    const DWORD length = ExpandEnvironmentStringsW(
        value.c_str(), expanded.data(), required);
    if (!length || length > required) return {};
    expanded.resize(length - 1);

    std::filesystem::path directory(expanded);
    if (directory.is_relative()) directory = relativeTo / directory;
    std::error_code error;
    directory = std::filesystem::weakly_canonical(directory, error);
    return !error && std::filesystem::is_directory(directory, error)
        ? directory : std::filesystem::path{};
}

std::filesystem::path ConfigFile(const std::wstring& reshadeDll) {
    const std::filesystem::path moduleDirectory =
        std::filesystem::path(reshadeDll).parent_path();
    const std::filesystem::path gameDirectory = moduleDirectory.parent_path();
    const std::wstring basePath = ReadIni(
        (gameDirectory / L"ReShade.ini").wstring(), L"INSTALL", L"BasePath");
    std::filesystem::path directory = ExistingDirectory(basePath, moduleDirectory);
    if (directory.empty()) {
        directory = ExistingDirectory(
            EnvironmentValue(L"RESHADE_BASE_PATH_OVERRIDE"), moduleDirectory);
    }
    if (directory.empty()) directory = moduleDirectory;
    return directory / L"ReShade.ini";
}

bool PointsToLocalDxvk(const std::wstring& config,
                       const std::wstring& dxvkDll) {
    if (ReadIni(config, L"PROXY", L"EnableProxyLibrary") != L"1") {
        return false;
    }
    std::filesystem::path proxy(ReadIni(config, L"PROXY", L"ProxyLibrary"));
    if (proxy.empty()) return false;
    if (proxy.is_relative()) {
        proxy = std::filesystem::path(config).parent_path() / proxy;
    }
    std::error_code error;
    return std::filesystem::equivalent(proxy, dxvkDll, error) && !error;
}

} // namespace

bool PrepareLocalDxvk(const std::wstring& reshadeDll,
                      const std::wstring& dxvkDll) {
    const DWORD dxvkAttributes = GetFileAttributesW(dxvkDll.c_str());
    if (reshadeDll.empty() || dxvkDll.empty() ||
        dxvkAttributes == INVALID_FILE_ATTRIBUTES ||
        (dxvkAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    const std::wstring config = ConfigFile(reshadeDll).wstring();
    if (PointsToLocalDxvk(config, dxvkDll)) return true;

    if (GetFileAttributesW(config.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring backup = config + L".NovaFix.bak";
        if (!CopyFileW(config.c_str(), backup.c_str(), TRUE) &&
            GetLastError() != ERROR_FILE_EXISTS) {
            LogWarning("ReShade + Local DXVK unavailable: could not back up %ls",
                       config.c_str());
            return false;
        }
    }

    const std::array<settings::ini::Entry, 2> entries{{
        {L"EnableProxyLibrary", L"1"},
        {L"ProxyLibrary", dxvkDll},
    }};
    if (!settings::ini::WriteValues(config, L"PROXY", entries) ||
        !PointsToLocalDxvk(config, dxvkDll)) {
        LogWarning("ReShade + Local DXVK unavailable: could not configure %ls",
                   config.c_str());
        return false;
    }

    LogInfo("ReShade proxy configured for Local DXVK: %ls -> %ls",
            config.c_str(), dxvkDll.c_str());
    return true;
}

} // namespace novafix::d3d9::reshade_chain
