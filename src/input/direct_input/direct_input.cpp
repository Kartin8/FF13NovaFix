#include "input/direct_input/direct_input.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "input/activity.h"
#include "input/direct_input/direct_input_diagnostics.h"
#include "input/direct_input/direct_input_state.h"
#include "overlay/input/overlay_hotkey.h"
#include "input/win32_input.h"
#include "overlay/overlay.h"
#include "patch/iat_hook.h"
#include "patch/vtable_hook.h"
#include "settings/input_settings.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace novafix::input {
namespace {

enum class DeviceKind : unsigned char { Unknown, Keyboard, Mouse, Controller };

enum class DeviceClaim : std::uint8_t {
    Release,
    GetState,
    GetData,
    Count,
};

constexpr std::size_t kDeviceClaimCount =
    static_cast<std::size_t>(DeviceClaim::Count);

using DirectInput8CreateFn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using DeviceReleaseFn = ULONG (STDMETHODCALLTYPE*)(IDirectInputDevice8A*);
using GetDeviceStateFn = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

struct DeviceRecord {
    IDirectInputDevice8A* object{};
    void** vtable{};
    DeviceKind kind{DeviceKind::Unknown};
    DirectInputStateTracker state;
    bool discardBufferedOnFocus{};
};

struct StateObservation {
    DeviceKind kind{DeviceKind::Unknown};
    bool controllerComparable{};
    bool controllerStateChanged{};
};

struct DeviceVtableRecord {
    void** vtable{};
    DeviceReleaseFn release{};
    GetDeviceStateFn getState{};
    GetDeviceDataFn getData{};
    std::array<patch::registry::ClaimId, kDeviceClaimCount> claims{};
};

struct RootVtableRecord {
    void** vtable{};
    CreateDeviceFn createDevice{};
};

DirectInput8CreateFn g_directInput8Create = nullptr;
std::vector<DeviceRecord> g_devices;
std::deque<DeviceVtableRecord> g_deviceVtables;
std::vector<RootVtableRecord> g_rootVtables;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic_bool g_reportedFocus{true};

direct_input_diagnostics::DeviceClass DiagnosticClass(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::Controller:
        return direct_input_diagnostics::DeviceClass::Controller;
    case DeviceKind::Keyboard:
        return direct_input_diagnostics::DeviceClass::Keyboard;
    case DeviceKind::Mouse:
        return direct_input_diagnostics::DeviceClass::Mouse;
    case DeviceKind::Unknown:
        return direct_input_diagnostics::DeviceClass::Unknown;
    }
    return direct_input_diagnostics::DeviceClass::Unknown;
}

patch::registry::ClaimId* Claim(
    DeviceVtableRecord& record, DeviceClaim claim) {
    return &record.claims[static_cast<std::size_t>(claim)];
}
DeviceKind FindDeviceKind(IDirectInputDevice8A* object) {
    DeviceKind result = DeviceKind::Unknown;
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_devices) {
        if (record.object == object) {
            result = record.kind;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return result;
}

StateObservation ObserveState(IDirectInputDevice8A* object, DWORD size,
                              const void* data,
                              bool classifyControllerState) {
    StateObservation observation{};
    if (!object || !data) return observation;

    DirectInputStateObservation state{};
    AcquireSRWLockExclusive(&g_lock);
    for (auto& record : g_devices) {
        if (record.object != object) continue;
        observation.kind = record.kind;
        if (observation.kind == DeviceKind::Keyboard) {
            state = record.state.ObserveKeyboard(
                object, size, data, static_cast<unsigned>(observation.kind));
        } else if (observation.kind == DeviceKind::Mouse) {
            state = record.state.ObserveMouse(
                object, size, data, static_cast<unsigned>(observation.kind));
        } else if (observation.kind == DeviceKind::Controller) {
            state = record.state.ObserveController(
                object, size, data, static_cast<unsigned>(observation.kind),
                classifyControllerState);
            observation.controllerComparable = state.comparable;
            observation.controllerStateChanged = state.changed;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (state.active) NotifyActivity(observation.kind == DeviceKind::Controller
        ? InputSource::Controller : InputSource::KeyboardMouse);
    return observation;
}

void SuppressState(IDirectInputDevice8A* object, DeviceKind kind,
                   DWORD size, void* data) {
    if (!data) return;
    if (kind == DeviceKind::Keyboard || kind == DeviceKind::Mouse) {
        ZeroMemory(data, size);
        return;
    }
    if (kind != DeviceKind::Controller) return;

    bool found = false;
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_devices) {
        if (record.object != object) continue;
        record.state.NeutralizeController(size, data);
        found = true;
        break;
    }
    ReleaseSRWLockShared(&g_lock);
    if (!found) DirectInputStateTracker{}.NeutralizeController(size, data);
}

bool IsTransientFocusFailure(HRESULT result) {
    return result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED ||
           result == DIERR_OTHERAPPHASPRIO;
}

struct BufferedDeviceState {
    DeviceKind kind{DeviceKind::Unknown};
    bool discard{};
};

BufferedDeviceState ConsumeBufferedDeviceState(
    IDirectInputDevice8A* object) {
    BufferedDeviceState result{};
    AcquireSRWLockExclusive(&g_lock);
    for (auto& record : g_devices) {
        if (record.object != object) continue;
        result.kind = record.kind;
        result.discard = record.discardBufferedOnFocus;
        record.discardBufferedOnFocus = false;
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

const DeviceVtableRecord& FindDeviceVtable(IDirectInputDevice8A* object) {
    static const DeviceVtableRecord empty{};
    void** table = object ? *reinterpret_cast<void***>(object) : nullptr;
    if (!table) return empty;
    // Cache the stable vtable record per thread to avoid locking on every poll
    thread_local void** cachedTable = nullptr;
    thread_local const DeviceVtableRecord* cachedRecord = nullptr;
    if (table == cachedTable && cachedRecord) return *cachedRecord;

    const DeviceVtableRecord* result = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_deviceVtables) {
        if (record.vtable == table) {
            result = &record;
            cachedTable = table;
            cachedRecord = &record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return result ? *result : empty;
}

void DeactivateDevice(IDirectInputDevice8A* object) {
    void** table = nullptr;
    std::array<patch::registry::ClaimId, kDeviceClaimCount> dormantClaims{};
    bool tableDormant = false;
    AcquireSRWLockExclusive(&g_lock);
    for (auto& record : g_devices) {
        if (record.object != object) continue;
        table = record.vtable;
        record = {};
        break;
    }
    if (table) {
        const bool stillActive = std::any_of(
            g_devices.begin(), g_devices.end(),
            [table](const DeviceRecord& record) {
                return record.object && record.vtable == table;
            });
        if (!stillActive) {
            for (const auto& record : g_deviceVtables) {
                if (record.vtable != table) continue;
                dormantClaims = record.claims;
                tableDormant = true;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (tableDormant) {
        for (const patch::registry::ClaimId claim : dormantClaims) {
            patch::registry::MarkDormant(claim);
        }
    }
}

ULONG STDMETHODCALLTYPE DeviceReleaseHook(IDirectInputDevice8A* self) {
    const auto& functions = FindDeviceVtable(self);
    if (!functions.release) return 0;
    const ULONG references = functions.release(self);
    if (references == 0) DeactivateDevice(self);
    return references;
}

HRESULT STDMETHODCALLTYPE GetDeviceStateHook(IDirectInputDevice8A* self, DWORD size, LPVOID data) {
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::DirectInputGetStateOwn);
    const auto& functions = FindDeviceVtable(self);
    if (!functions.getState) return DIERR_NOTINITIALIZED;
    const DeviceKind knownKind = FindDeviceKind(self);
    auto diagnostic = direct_input_diagnostics::BeginStateCall();
    const HRESULT result = functions.getState(self, size, data);
    direct_input_diagnostics::EndStateCall(
        diagnostic, DiagnosticClass(knownKind), FAILED(result),
        IsTransientFocusFailure(result));
    const bool suppressed = GameInputSuppressed() ||
        (knownKind == DeviceKind::Controller &&
         overlay::CapturesControllerInput());
    if (suppressed && data &&
        (SUCCEEDED(result) || IsTransientFocusFailure(result))) {
        if (knownKind == DeviceKind::Unknown) return result;
        SuppressState(self, knownKind, size, data);
        return DI_OK;
    }
    if (FAILED(result) || !data) return result;

    StateObservation observation{};
    {
        hook_profiler::Scope observeTiming(
            hook_profiler::Point::DirectInputObserveState);
        observation = ObserveState(
            self, size, data, diagnostic.active);
    }
    if (observation.kind == DeviceKind::Controller &&
        observation.controllerComparable) {
        direct_input_diagnostics::RecordControllerState(
            diagnostic, observation.controllerStateChanged);
    }
    if (observation.kind == DeviceKind::Keyboard) {
        const settings::InputSettings inputSettings = settings::ActiveInput();
        const unsigned primary =
            OverlayHotkeyDirectInputKey(inputSettings.overlayHotkey);
        const unsigned alternate =
            OverlayHotkeyDirectInputKey(inputSettings.alternateOverlayHotkey);
        if (primary < size) static_cast<unsigned char*>(data)[primary] = 0;
        if (alternate < size) static_cast<unsigned char*>(data)[alternate] = 0;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE GetDeviceDataHook(IDirectInputDevice8A* self, DWORD objectSize,
                                             LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    hook_profiler::Scope totalTiming(
        hook_profiler::Point::DirectInputGetDataOwn);
    const auto& functions = FindDeviceVtable(self);
    if (!functions.getData) return DIERR_NOTINITIALIZED;
    HRESULT result = DIERR_NOTINITIALIZED;
    {
        hook_profiler::Scope originalTiming(
            hook_profiler::Point::DirectInputGetDataOriginal);
        result = functions.getData(
            self, objectSize, data, count, flags);
    }
    const DeviceKind knownKind = FindDeviceKind(self);
    const bool suppressed = GameInputSuppressed() ||
        (knownKind == DeviceKind::Controller &&
         overlay::CapturesControllerInput());
    if (suppressed && count &&
        (SUCCEEDED(result) || IsTransientFocusFailure(result))) {
        *count = 0;
        return DI_OK;
    }
    if (FAILED(result) || !count) return result;

    const BufferedDeviceState deviceState =
        ConsumeBufferedDeviceState(self);
    const DeviceKind kind = deviceState.kind;
    const settings::InputSettings inputSettings = kind == DeviceKind::Keyboard
        ? settings::ActiveInput() : settings::InputSettings{};
    if (deviceState.discard) {
        *count = 0;
        return result;
    }

    bool active = false;
    if (data && objectSize >= sizeof(DIDEVICEOBJECTDATA)) {
        for (DWORD index = 0; index < *count; ++index) {
            const auto* item = reinterpret_cast<const DIDEVICEOBJECTDATA*>(
                reinterpret_cast<const unsigned char*>(data) + index * objectSize);
            switch (kind) {
            case DeviceKind::Keyboard:
                active = active ||
                    (!IsOverlayHotkeyDirectInputKey(
                         item->dwOfs, inputSettings) &&
                     (item->dwData & 0x80u) != 0);
                break;
            case DeviceKind::Mouse:
                active = active || item->dwData != 0;
                break;
            case DeviceKind::Controller:
                active = active ||
                    (item->dwOfs >= DIJOFS_BUTTON0 &&
                     item->dwOfs <= DIJOFS_BUTTON31 &&
                     (item->dwData & 0x80u) != 0);
                break;
            case DeviceKind::Unknown:
                break;
            }
        }
    }
    if (active) NotifyActivity(kind == DeviceKind::Controller
        ? InputSource::Controller : InputSource::KeyboardMouse);

    if (kind == DeviceKind::Keyboard && data && objectSize >= sizeof(DIDEVICEOBJECTDATA)) {
        DWORD outputIndex = 0;
        for (DWORD inputIndex = 0; inputIndex < *count; ++inputIndex) {
            auto* item = reinterpret_cast<DIDEVICEOBJECTDATA*>(
                reinterpret_cast<unsigned char*>(data) + inputIndex * objectSize);
            if (IsOverlayHotkeyDirectInputKey(item->dwOfs, inputSettings)) continue;
            if (outputIndex != inputIndex) {
                std::memmove(reinterpret_cast<unsigned char*>(data) + outputIndex * objectSize,
                             item, objectSize);
            }
            ++outputIndex;
        }
        *count = outputIndex;
    }
    return result;
}

void RegisterDevice(IDirectInputDevice8A* device, DeviceKind kind) {
    if (!device || kind == DeviceKind::Unknown) return;
    void** table = *reinterpret_cast<void***>(device);

    AcquireSRWLockExclusive(&g_lock);
    bool deviceRegistered = false;
    for (auto& record : g_devices) {
        if (!record.object || record.object == device) {
            record.object = device;
            record.vtable = table;
            record.kind = kind;
            record.state.Reset();
            record.discardBufferedOnFocus = !g_reportedFocus.load(std::memory_order_relaxed);
            deviceRegistered = true;
            break;
        }
    }
    if (!deviceRegistered) {
        g_devices.emplace_back();
        DeviceRecord& record = g_devices.back();
        record.object = device;
        record.vtable = table;
        record.kind = kind;
        record.discardBufferedOnFocus =
            !g_reportedFocus.load(std::memory_order_relaxed);
    }

    DeviceVtableRecord* vtableRecord = nullptr;
    for (auto& record : g_deviceVtables) {
        if (record.vtable == table) {
            vtableRecord = &record;
            break;
        }
    }
    if (!vtableRecord) {
        g_deviceVtables.emplace_back();
        vtableRecord = &g_deviceVtables.back();
    }

    if (vtableRecord->vtable) {
        for (const patch::registry::ClaimId claim : vtableRecord->claims) {
            patch::registry::MarkApplied(claim);
        }
    }

    if (!vtableRecord->vtable) {
        vtableRecord->vtable = table;
        void* originalRelease = nullptr;
        void* originalState = nullptr;
        void* originalData = nullptr;
        const bool releaseOk = ReplaceVtableSlot(
            "direct-input-device:release", table, 2,
            reinterpret_cast<void*>(&DeviceReleaseHook), &originalRelease,
            Claim(*vtableRecord, DeviceClaim::Release));
        const bool stateOk = ReplaceVtableSlot(
            "direct-input-device:get-state", table, 9,
            reinterpret_cast<void*>(&GetDeviceStateHook), &originalState,
            Claim(*vtableRecord, DeviceClaim::GetState));
        const bool dataOk = ReplaceVtableSlot(
            "direct-input-device:get-data", table, 10,
            reinterpret_cast<void*>(&GetDeviceDataHook), &originalData,
            Claim(*vtableRecord, DeviceClaim::GetData));
        if (releaseOk) vtableRecord->release = reinterpret_cast<DeviceReleaseFn>(originalRelease);
        if (stateOk) vtableRecord->getState = reinterpret_cast<GetDeviceStateFn>(originalState);
        if (dataOk) vtableRecord->getData = reinterpret_cast<GetDeviceDataFn>(originalData);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

CreateDeviceFn FindCreateDevice(IDirectInput8A* object) {
    CreateDeviceFn result = nullptr;
    void** table = object ? *reinterpret_cast<void***>(object) : nullptr;
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_rootVtables) {
        if (record.vtable == table) {
            result = record.createDevice;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return result;
}

HRESULT STDMETHODCALLTYPE CreateDeviceHook(IDirectInput8A* self, REFGUID guid,
                                            LPDIRECTINPUTDEVICE8A* output, LPUNKNOWN outer) {
    const CreateDeviceFn original = FindCreateDevice(self);
    if (!original) return DIERR_NOTINITIALIZED;
    const HRESULT result = original(self, guid, output, outer);
    if (SUCCEEDED(result) && output && *output) {
        DeviceKind kind = DeviceKind::Controller;
        if (IsEqualGUID(guid, GUID_SysKeyboard)) kind = DeviceKind::Keyboard;
        if (IsEqualGUID(guid, GUID_SysMouse) || IsEqualGUID(guid, GUID_SysMouseEm)) kind = DeviceKind::Mouse;
        RegisterDevice(*output, kind);
        if (kind == DeviceKind::Controller) {
            direct_input_diagnostics::LogControllerIdentity(*output);
        }
        Log("DirectInput device: object=%p kind=%u guid=%08lX-%04X-%04X", *output,
            static_cast<unsigned>(kind), guid.Data1, guid.Data2, guid.Data3);
    }
    return result;
}

HRESULT WINAPI DirectInput8CreateHook(HINSTANCE instance, DWORD version, REFIID iid,
                                      LPVOID* output, LPUNKNOWN outer) {
    if (!g_directInput8Create) return E_FAIL;
    const HRESULT result = g_directInput8Create(instance, version, iid, output, outer);
    if (FAILED(result) || !output || !*output) return result;

    auto** table = *reinterpret_cast<void***>(*output);
    AcquireSRWLockExclusive(&g_lock);
    RootVtableRecord* record = nullptr;
    for (auto& candidate : g_rootVtables) {
        if (candidate.vtable == table) {
            record = &candidate;
            break;
        }
        if (!candidate.vtable && !record) record = &candidate;
    }
    if (!record) {
        g_rootVtables.emplace_back();
        record = &g_rootVtables.back();
    }
    if (!record->vtable) {
        void* original = nullptr;
        if (ReplaceVtableSlot(
                "direct-input8:create-device", table, 3,
                reinterpret_cast<void*>(&CreateDeviceHook), &original)) {
            record->vtable = table;
            record->createDevice = reinterpret_cast<CreateDeviceFn>(original);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

} // namespace

void InstallDirectInputHooks() {
    PatchExecutableImport(
        "dinput8.dll", "DirectInput8Create",
        reinterpret_cast<void*>(&DirectInput8CreateHook),
        reinterpret_cast<void**>(&g_directInput8Create));
}

void NotifyDirectInputFocusChanged(bool focused) {
    const bool previous = g_reportedFocus.exchange(focused, std::memory_order_acq_rel);
    if (previous == focused) return;
    if (!focused) {
        AcquireSRWLockExclusive(&g_lock);
        for (auto& record : g_devices) {
            if (record.object) record.discardBufferedOnFocus = true;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    Log("Game input: state=%s focus=%s",
        focused ? "active" : "suspended",
        focused ? "gained" : "lost");
}

} // namespace novafix::input
