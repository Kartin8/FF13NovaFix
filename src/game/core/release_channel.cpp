#include "game/core/release_channel.h"

#include "compat/platform.h"

#include <windows.h>

namespace novafix::game {

ReleaseChannel DetectReleaseChannel() {
    // Wine/Proton games are desktop installs. Avoid probing an optional package
    // API that older compatibility layers may only expose as a stub
    if (compat::IsWine()) return ReleaseChannel::Unknown;

    using GetCurrentPackageFullNameFn = LONG (WINAPI*)(UINT32*, PWSTR);

    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto getCurrentPackageFullName = kernel
        ? reinterpret_cast<GetCurrentPackageFullNameFn>(
              GetProcAddress(kernel, "GetCurrentPackageFullName"))
        : nullptr;
    if (!getCurrentPackageFullName) return ReleaseChannel::Unknown;

    UINT32 length = 0;
    return getCurrentPackageFullName(&length, nullptr) ==
            ERROR_INSUFFICIENT_BUFFER
        ? ReleaseChannel::MicrosoftStore
        : ReleaseChannel::Unknown;
}

const char* ReleaseChannelName(ReleaseChannel channel) {
    switch (channel) {
    case ReleaseChannel::Steam: return "Steam";
    case ReleaseChannel::MicrosoftStore: return "Microsoft Store";
    case ReleaseChannel::Unknown: return "unidentified desktop release";
    }
    return "unidentified desktop release";
}

} // namespace novafix::game
