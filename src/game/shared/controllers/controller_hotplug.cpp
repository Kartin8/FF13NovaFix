#include "game/shared/controllers/controller_hotplug.h"

#include "compat/platform.h"
#include "diagnostics/log.h"
#include "game/shared/controllers/controller_hotplug_backend.h"
#include "game/core/features.h"
#include "game/core/game_profile.h"
#include "input/xinput_backend.h"
#include "settings/performance_settings.h"

#include <windows.h>
#include <cfgmgr32.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace novafix::game::controller_hotplug {
namespace {

// These are offsets from the start of a real topology event, not a permanent
// poll. XIII DirectInput backends need a few chances for Steam Input or a
// Bluetooth transport to finish publishing its XInput slot
constexpr std::array<DWORD, 6> kTopologyProbeScheduleMs{
    0u, 125u, 250u, 500u, 1000u, 2000u,
};
constexpr ULONGLONG kSlowProbeLogIntervalMs = 30000u;

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_available{false};
std::atomic_uint g_lastXInputMask{};
std::atomic_bool g_xinputBaselineReady{false};
std::atomic_uint64_t g_lastSlowXinputLogAt{};
HANDLE g_topologyProbeEvent{};
HCMNOTIFICATION g_configurationManagerNotification{};
bool g_previousSmartHotplugEnabled{};

void RequestTopologyProbe() {
    if (g_topologyProbeEvent) SetEvent(g_topologyProbeEvent);
}

void ObserveXInputTopology() {
    if (!g_available.load(std::memory_order_acquire) ||
        !settings::ActivePerformance().smartControllerHotplug) {
        g_xinputBaselineReady.store(false, std::memory_order_release);
        return;
    }

    const ULONGLONG started = GetTickCount64();
    const unsigned connected = input::xinput::ConnectedMask();
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed >= 8u) {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG previous =
            g_lastSlowXinputLogAt.load(std::memory_order_relaxed);
        if (now - previous >= kSlowProbeLogIntervalMs &&
            g_lastSlowXinputLogAt.compare_exchange_strong(
                previous, now, std::memory_order_relaxed)) {
            LogWarning(
                "Slow event-triggered XInput topology probe: duration=%llums mask=0x%X",
                static_cast<unsigned long long>(elapsed), connected);
        }
    }

    if (!g_xinputBaselineReady.exchange(true, std::memory_order_acq_rel)) {
        g_lastXInputMask.store(connected, std::memory_order_release);
        return;
    }

    const unsigned previous =
        g_lastXInputMask.exchange(connected, std::memory_order_acq_rel);
    if (connected == previous) return;

    LogInfo("Controller connection changed");
    Log("XInput mask: 0x%X -> 0x%X", previous, connected);
    features::NotifyDeviceTopologyChanged();
}

DWORD WINAPI XInputTopologyWatcher(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        if (!g_topologyProbeEvent ||
            WaitForSingleObject(g_topologyProbeEvent, INFINITE) !=
                WAIT_OBJECT_0) {
            return 0u;
        }

        DWORD previousOffset{};
        std::size_t index{};
        while (index < kTopologyProbeScheduleMs.size()) {
            const DWORD offset = kTopologyProbeScheduleMs[index];
            const DWORD delay = offset - previousOffset;
            if (delay != 0u &&
                WaitForSingleObject(g_topologyProbeEvent, delay) ==
                    WAIT_OBJECT_0) {
                index = 0u;
                previousOffset = 0u;
                continue;
            }
            ObserveXInputTopology();
            previousOffset = offset;
            ++index;
        }
    }
}

DWORD CALLBACK ConfigurationManagerNotification(
    HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (action != CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL &&
        action != CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        return ERROR_SUCCESS;
    }

    Log("Controller Plug-and-Play event received: action=%s",
        action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL
            ? "arrival"
            : "removal");
    // Dispatches to both native hotplug and vibration target invalidation
    features::NotifyDeviceTopologyChanged();
    return ERROR_SUCCESS;
}

bool RegisterConfigurationManagerNotifications() {
    if (compat::IsWine()) {
        Log("Controller Plug-and-Play callback skipped on Wine/Proton: Win32 device messages remain active");
        return false;
    }

    using RegisterNotificationFn = CONFIGRET (WINAPI*)(
        PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK,
        PHCMNOTIFICATION);

    const HMODULE module = LoadLibraryW(L"cfgmgr32.dll");
    if (!module) {
        LogWarning(
            "Controller Plug-and-Play callback unavailable: cfgmgr32 load failed error=%lu Win32-device-messages=active",
            GetLastError());
        return false;
    }
    const auto registerNotification =
        reinterpret_cast<RegisterNotificationFn>(
            GetProcAddress(module, "CM_Register_Notification"));
    if (!registerNotification) {
        FreeLibrary(module);
        LogWarning(
            "Controller Plug-and-Play callback unavailable on this Windows version: Win32 device messages remain active");
        return false;
    }

    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    const CONFIGRET result = registerNotification(
        &filter, nullptr, &ConfigurationManagerNotification,
        &g_configurationManagerNotification);
    if (result != CR_SUCCESS) {
        FreeLibrary(module);
        LogWarning(
            "Controller Plug-and-Play callback registration failed: result=0x%X Win32-device-messages=active",
            static_cast<unsigned>(result));
        return false;
    }

    // The callback belongs to cfgmgr32; retain the module for the process
    // lifetime together with the notification registration
    return true;
}

void MaintainSettingState() {
    const bool enabled = settings::ActivePerformance().smartControllerHotplug;
    if (enabled && !g_previousSmartHotplugEnabled) {
        backend::NotifyTopologyChanged();
        RequestTopologyProbe();
    }
    g_previousSmartHotplugEnabled = enabled;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (!Supports(Capability::ControllerHotplug) || !backend::Initialize()) {
        LogWarning("Smart controller hotplug disabled: unsupported native backend");
        return TRUE;
    }

    g_previousSmartHotplugEnabled =
        settings::ActivePerformance().smartControllerHotplug;
    g_available.store(true, std::memory_order_release);
    (void)RegisterConfigurationManagerNotifications();

    if (backend::RequiresXInputVerification()) {
        g_topologyProbeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE watcher = g_topologyProbeEvent
            ? CreateThread(nullptr, 0, &XInputTopologyWatcher, nullptr, 0,
                           nullptr)
            : nullptr;
        if (watcher) {
            CloseHandle(watcher);
            RequestTopologyProbe();
        } else {
            const DWORD error = GetLastError();
            if (g_topologyProbeEvent) {
                CloseHandle(g_topologyProbeEvent);
                g_topologyProbeEvent = nullptr;
            }
            LogWarning(
                "Smart controller hotplug enabled without XInput verification worker: error=%lu native-event-reconnect=active",
                error);
        }
    }

    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

bool Available() {
    return g_available.load(std::memory_order_acquire);
}

void Pump() {
    if (!g_available.load(std::memory_order_acquire)) return;
    MaintainSettingState();
    backend::Pump();
}

void NotifyDeviceTopologyChanged() {
    if (!g_available.load(std::memory_order_acquire) ||
        !settings::ActivePerformance().smartControllerHotplug) {
        return;
    }
    backend::NotifyTopologyChanged();
    RequestTopologyProbe();
}

} // namespace novafix::game::controller_hotplug
