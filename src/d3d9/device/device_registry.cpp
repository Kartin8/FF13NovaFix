#include "d3d9/device/device_registry.h"

#include "d3d9/device/device_shader_hooks.h"
#include "d3d9/device/device_state_hooks.h"
#include "display/aspect_runtime.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <vector>

namespace novafix::d3d9::device_registry {
namespace {

std::vector<DeviceRecord> g_devices;
std::deque<DeviceVtableRecord> g_deviceVtables;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic_uint64_t g_deviceGeneration{1};

void PublishDeviceExtent(
    IDirect3DDevice9* device,
    const D3DPRESENT_PARAMETERS* parameters) {
    UINT width = parameters ? parameters->BackBufferWidth : 0u;
    UINT height = parameters ? parameters->BackBufferHeight : 0u;
    IDirect3DSurface9* backBuffer = nullptr;
    if (device && SUCCEEDED(device->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
        backBuffer) {
        D3DSURFACE_DESC description{};
        if (SUCCEEDED(backBuffer->GetDesc(&description))) {
            width = description.Width;
            height = description.Height;
        }
        backBuffer->Release();
    }
    display::PublishBackBufferExtent(width, height);
}

DeviceVtableRecord* RecordDevice(
    IDirect3DDevice9* device, void** table, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended) {
    bool deviceRecorded = false;
    for (auto& record : g_devices) {
        if (!record.device || record.device == device) {
            record.device = device;
            record.vtable = table;
            record.window = window;
            record.forcedProfile = forcedProfile;
            record.extended = extended;
            record.backBufferWidth = parameters ? parameters->BackBufferWidth : 0;
            record.backBufferHeight = parameters ? parameters->BackBufferHeight : 0;
            deviceRecorded = true;
            break;
        }
    }
    if (!deviceRecorded) {
        g_devices.push_back({
            device, table, window, forcedProfile, extended,
            parameters ? parameters->BackBufferWidth : 0,
            parameters ? parameters->BackBufferHeight : 0,
        });
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
    return vtableRecord;
}

} // namespace

DeviceRecord FindDevice(IDirect3DDevice9* device) {
    thread_local IDirect3DDevice9* cachedDevice = nullptr;
    thread_local std::uint64_t cachedGeneration{};
    thread_local DeviceRecord cachedRecord{};
    const std::uint64_t generation =
        g_deviceGeneration.load(std::memory_order_acquire);
    if (device == cachedDevice && cachedGeneration == generation) {
        return cachedRecord;
    }

    DeviceRecord result{};
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_devices) {
        if (record.device == device) {
            result = record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    cachedDevice = device;
    cachedGeneration = generation;
    cachedRecord = result;
    return result;
}

const DeviceVtableRecord& FindDeviceVtable(IDirect3DDevice9* device) {
    static const DeviceVtableRecord empty{};
    void** table = device ? *reinterpret_cast<void***>(device) : nullptr;
    if (!table) return empty;

    // Device vtable records live for the whole process. Cache the last one per thread to avoid locking/scanning on every draw or state change.
    thread_local void** cachedTable = nullptr;
    thread_local const DeviceVtableRecord* cachedRecord = nullptr;
    if (table == cachedTable && cachedRecord) return *cachedRecord;

    const DeviceVtableRecord* result = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (const auto& record : g_deviceVtables) {
        if (record.vtable == table) {
            result = &record;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (result) {
        cachedTable = table;
        cachedRecord = result;
    }
    return result ? *result : empty;
}

void DeactivateDevice(IDirect3DDevice9* device) {
    void** table = nullptr;
    std::array<patch::registry::ClaimId, kDeviceClaimCount> dormantClaims{};
    bool tableDormant = false;

    AcquireSRWLockExclusive(&g_lock);
    for (auto& record : g_devices) {
        if (record.device != device) continue;
        table = record.vtable;
        record = {};
        break;
    }
    if (table) {
        const bool stillActive = std::any_of(
            g_devices.begin(), g_devices.end(),
            [table](const DeviceRecord& record) {
                return record.device && record.vtable == table;
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
    g_deviceGeneration.fetch_add(1u, std::memory_order_release);

    if (tableDormant) {
        for (const patch::registry::ClaimId claim : dormantClaims) {
            patch::registry::MarkDormant(claim);
        }
        device_shader_hooks::MarkDormant(table);
        device_state_hooks::MarkDormant(table);
    }
    // Keep saved originals so DXVK can reuse its discovery vtable. Integrity
    // is suspended only while no registered live device owns the table
}

void UpdateDevicePresentation(IDirect3DDevice9* device,
                              const D3DPRESENT_PARAMETERS* parameters) {
    if (!device || !parameters) return;
    PublishDeviceExtent(device, parameters);
    AcquireSRWLockExclusive(&g_lock);
    for (auto& record : g_devices) {
        if (record.device == device) {
            record.window = parameters->hDeviceWindow ? parameters->hDeviceWindow : record.window;
            record.backBufferWidth = parameters->BackBufferWidth;
            record.backBufferHeight = parameters->BackBufferHeight;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_deviceGeneration.fetch_add(1u, std::memory_order_release);
}

void RegisterDevice(
    IDirect3DDevice9* device, void** table, HWND window, bool forcedProfile,
    const D3DPRESENT_PARAMETERS* parameters, bool extended,
    const DeviceHookPolicy& hookPolicy, InstallHooksFn installHooks) {
    AcquireSRWLockExclusive(&g_lock);
    DeviceVtableRecord* record = RecordDevice(
        device, table, window, forcedProfile, parameters, extended);
    installHooks(device, table, record, hookPolicy, extended);
    ReleaseSRWLockExclusive(&g_lock);
    g_deviceGeneration.fetch_add(1u, std::memory_order_release);
    PublishDeviceExtent(device, parameters);
}

} // namespace novafix::d3d9::device_registry
