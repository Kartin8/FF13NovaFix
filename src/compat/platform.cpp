#include "compat/platform.h"

#include "diagnostics/log.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace novafix::compat {
namespace {

using WineGetVersion = const char* (__cdecl*)();
using WineGetHostVersion = void (__cdecl*)(const char**, const char**);

bool EqualsIgnoreCase(std::string left, const char* right) {
    std::transform(left.begin(), left.end(), left.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return left == right;
}

Environment DetectEnvironment() {
    Environment result{};
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return result;

    const auto getVersion = reinterpret_cast<WineGetVersion>(GetProcAddress(ntdll, "wine_get_version"));
    if (!getVersion) return result;

    if (const char* version = getVersion()) result.wineVersion = version;
    result.host = Host::WineOther;
    result.hostName = "unknown";

    const auto getHostVersion =
        reinterpret_cast<WineGetHostVersion>(GetProcAddress(ntdll, "wine_get_host_version"));
    if (getHostVersion) {
        const char* system = nullptr;
        const char* release = nullptr;
        getHostVersion(&system, &release);
        if (system) result.hostName = system;
        if (release) result.hostRelease = release;
    }

    if (EqualsIgnoreCase(result.hostName, "linux")) result.host = Host::WineLinux;
    if (EqualsIgnoreCase(result.hostName, "darwin")) result.host = Host::WineMacOS;
    return result;
}

} // namespace

const Environment& CurrentEnvironment() {
    static const Environment environment = DetectEnvironment();
    return environment;
}

bool IsWine() {
    return CurrentEnvironment().host != Host::Windows;
}

const char* HostName(Host host) {
    switch (host) {
    case Host::Windows: return "Windows";
    case Host::WineLinux: return "Wine/Proton on Linux";
    case Host::WineMacOS: return "Wine on macOS";
    case Host::WineOther: return "Wine on another host";
    }
    return "Unknown";
}

void LogEnvironment() {
    const Environment& environment = CurrentEnvironment();
    if (environment.host == Host::Windows) {
        return;
    }
    Log("Compatibility environment: %s; Wine=%s; host=%s; release=%s",
        HostName(environment.host), environment.wineVersion.empty() ? "unknown" : environment.wineVersion.c_str(),
        environment.hostName.c_str(), environment.hostRelease.empty() ? "unknown" : environment.hostRelease.c_str());
}

} // namespace novafix::compat
