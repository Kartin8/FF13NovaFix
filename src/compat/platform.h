#pragma once

#include <string>

namespace novafix::compat {

enum class Host {
    Windows,
    WineLinux,
    WineMacOS,
    WineOther,
};

struct Environment {
    Host host{Host::Windows};
    std::string wineVersion;
    std::string hostName{"Windows"};
    std::string hostRelease;
};

const Environment& CurrentEnvironment();
bool IsWine();
const char* HostName(Host host);
void LogEnvironment();

} // namespace novafix::compat
