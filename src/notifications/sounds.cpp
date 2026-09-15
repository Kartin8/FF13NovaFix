#include "notifications/sounds.h"

#include "diagnostics/log.h"
#include "settings/performance_settings.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>

namespace novafix::notifications::sounds {
namespace {

constexpr WORD kStartupSoundResource = 102u;
constexpr WORD kMenuOpenSoundResource = 103u;
constexpr WORD kMenuCloseSoundResource = 104u;
constexpr ULONGLONG kStartupDelayMilliseconds = 1000u;

std::atomic<ULONGLONG> g_firstSuccessfulPresent{};
std::atomic_bool g_startupFinished{};
std::atomic_bool g_loggedFailure{};

bool GameOwnsForegroundWindow() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD processId{};
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

HMODULE ModuleHandle() {
    HMODULE module{};
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleHandle), &module);
    return module;
}

bool PlayEmbedded(WORD resource, bool preserveExistingSound) {
    const HMODULE module = ModuleHandle();
    const DWORD flags = SND_ASYNC | SND_RESOURCE | SND_NODEFAULT |
        (preserveExistingSound ? SND_NOSTOP : 0u);
    if (module && PlaySoundW(MAKEINTRESOURCEW(resource), module, flags)) {
        return true;
    }
    if (!g_loggedFailure.exchange(true, std::memory_order_acq_rel)) {
        LogWarning("Embedded UI sound could not be played");
    }
    return false;
}

} // namespace

void OnSuccessfulPresent() {
    if (g_startupFinished.load(std::memory_order_acquire)) return;
    if (!settings::ActivePerformance().startupSound) {
        g_startupFinished.store(true, std::memory_order_release);
        return;
    }

    const ULONGLONG now = GetTickCount64();
    ULONGLONG first = g_firstSuccessfulPresent.load(
        std::memory_order_acquire);
    if (first == 0u) {
        g_firstSuccessfulPresent.compare_exchange_strong(
            first, now, std::memory_order_acq_rel);
        return;
    }
    if (now - first < kStartupDelayMilliseconds ||
        !GameOwnsForegroundWindow()) {
        return;
    }

    bool expected = false;
    if (!g_startupFinished.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (PlayEmbedded(kStartupSoundResource, true)) {
        Log("Startup sound played");
    }
}

void PlayMenuTransition(bool opening) {
    if (!settings::ActivePerformance().menuSounds) return;
    (void)PlayEmbedded(opening ? kMenuOpenSoundResource
                               : kMenuCloseSoundResource,
                       false);
}

} // namespace novafix::notifications::sounds
