#include "game/shared/controllers/controller_hotplug_backend.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "input/hotplug_gate.h"
#include "patch/detour32.h"
#include "settings/performance_settings.h"

#include <windows.h>
#include <unknwn.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace novafix::game::controller_hotplug::backend {
namespace {

constexpr ULONGLONG kRetryIntervalMs = 750;
constexpr ULONGLONG kRetryWindowMs = 6000;

constexpr std::array<std::byte, 7> kFf13ScanPrefix{
    std::byte{0x55}, std::byte{0x8B}, std::byte{0xEC},
    std::byte{0x51}, std::byte{0x89}, std::byte{0x4D}, std::byte{0xFC},
};
constexpr std::array<std::byte, 10> kFf13_2ScanPrefix{
    std::byte{0x83}, std::byte{0x3D},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x56}, std::byte{0x8B}, std::byte{0xF1},
};
constexpr std::array<std::byte, 10> kLrRescanRequestPrefix{
    std::byte{0xE8}, std::byte{0x7B}, std::byte{0xF1}, std::byte{0xFF},
    std::byte{0xFF}, std::byte{0xE9}, std::byte{0x56}, std::byte{0x13},
    std::byte{0x00}, std::byte{0x00},
};

enum class Strategy {
    ValidatedDirectInputList,
    NativeAsyncRequest,
};

struct DirectInputListContract {
    std::uint32_t devicesRva;
    std::uint32_t countRva;
    int maximumDevices;
    std::size_t countOperandOffset;
};

struct Contract {
    Title title;
    const char* titleName;
    Strategy strategy;
    std::uint32_t entryRva;
    std::span<const std::byte> entryPrefix;
    std::optional<DirectInputListContract> directInputList;
};

constexpr std::array<Contract, 3> kContracts{{
    {
        Title::FinalFantasyXIII,
        "XIII",
        Strategy::ValidatedDirectInputList,
        0x422690u,
        kFf13ScanPrefix,
        DirectInputListContract{0x2411564u, 0x2411574u, 2, 9u},
    },
    {
        Title::FinalFantasyXIII2,
        "XIII-2",
        Strategy::ValidatedDirectInputList,
        0x2A7560u,
        kFf13_2ScanPrefix,
        DirectInputListContract{0x212A4A0u, 0x212A4B0u, 2, 2u},
    },
    {
        Title::LightningReturns,
        "LR",
        Strategy::NativeAsyncRequest,
        0x3439F0u,
        kLrRescanRequestPrefix,
        std::nullopt,
    },
}};

using DeviceListScanFn = bool (__thiscall*)(void* inputManager);
using NativeRescanRequestFn = void (__cdecl*)();

const Contract* g_contract{};
patch::Detour32 g_scanDetour;
DeviceListScanFn g_originalScan{};
NativeRescanRequestFn g_nativeRescanRequest{};
IUnknown** g_nativeDevices{};
int* g_nativeDeviceCount{};
input::HotplugGate g_gate{kRetryIntervalMs, kRetryWindowMs};
std::atomic_bool g_available{false};
std::atomic_bool g_rebuildRequested{false};
std::atomic_bool g_nativeRequestPending{false};

const Contract* ContractFor(Title title) {
    for (const Contract& contract : kContracts) {
        if (contract.title == title) return &contract;
    }
    return nullptr;
}

const char* TriggerName(input::HotplugTrigger trigger) {
    switch (trigger) {
    case input::HotplugTrigger::DeviceChange: return "device-change";
    case input::HotplugTrigger::Retry: return "bounded-retry";
    case input::HotplugTrigger::None: return "none";
    }
    return "unknown";
}

bool ValidateCountOperand(const ImageView& image,
                          const Contract& contract,
                          const DirectInputListContract& directInputList,
                          std::span<std::byte> expectedPrefix) {
    const auto countAddress = image.Address32(directInputList.countRva);
    if (!countAddress) return false;

    if (directInputList.countOperandOffset + sizeof(*countAddress) <=
        expectedPrefix.size()) {
        std::memcpy(expectedPrefix.data() + directInputList.countOperandOffset,
                    &*countAddress, sizeof(*countAddress));
        return true;
    }

    const auto operand = image.At(
        contract.entryRva + directInputList.countOperandOffset,
        sizeof(*countAddress));
    return operand &&
           std::memcmp(operand->data(), &*countAddress,
                       sizeof(*countAddress)) == 0;
}

bool RebuildDirectInputDeviceList(void* inputManager) {
    if (!g_contract || !g_contract->directInputList || !inputManager ||
        !g_nativeDevices || !g_nativeDeviceCount) {
        LogWarning(
            "Controller reconnect skipped native device-list rebuild: contract state unavailable");
        return false;
    }

    const DirectInputListContract& directInputList =
        *g_contract->directInputList;
    const int count = *g_nativeDeviceCount;
    if (count < 0 || count > directInputList.maximumDevices) {
        LogWarning(
            "Controller reconnect skipped native device-list rebuild: title=%s invalid-count=%d",
            g_contract->titleName, count);
        return false;
    }

    for (int index = 0; index < count; ++index) {
        IUnknown* device = g_nativeDevices[index];
        if (device) {
            // Mirrors the title's own DirectInput list destructor
            device->Release();
            g_nativeDevices[index] = nullptr;
        }
    }
    for (int index = count; index < directInputList.maximumDevices; ++index) {
        g_nativeDevices[index] = nullptr;
    }

    *g_nativeDeviceCount = 0;
    *static_cast<int*>(inputManager) = -1;
    Log("Controller device list rebuilt: title=%s released=%d",
        g_contract->titleName, count);
    return true;
}

bool __fastcall DeviceListScanHook(void* inputManager, void*) {
    NOVAFIX_PROFILE_NAMED_SCOPE(
        hookTiming, "hook.controller-device-list-scan-total");
    DeviceListScanFn original = g_originalScan;
    if (!original) return false;
    if (!settings::ActivePerformance().smartControllerHotplug) {
        return original(inputManager);
    }

    const input::HotplugTrigger trigger = g_gate.Consume(GetTickCount64());
    if (trigger == input::HotplugTrigger::None) return false;

    if (g_rebuildRequested.exchange(false, std::memory_order_acq_rel) &&
        !RebuildDirectInputDeviceList(inputManager)) {
        return false;
    }

    const ULONGLONG started = GetTickCount64();
    const bool found = original(inputManager);
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (found) g_gate.Resolve();
    Log("Controller hotplug scan: title=%s trigger=%s found=%d duration=%llums",
        g_contract->titleName, TriggerName(trigger), found,
        static_cast<unsigned long long>(elapsed));
    return found;
}

bool InitializeDirectInputList(const ImageView& image,
                               const Contract& contract,
                               std::span<const std::byte> expected) {
    const DirectInputListContract& directInputList =
        *contract.directInputList;
    const auto devices = image.At(
        directInputList.devicesRva,
        sizeof(IUnknown*) *
            static_cast<std::size_t>(directInputList.maximumDevices));
    const auto count = image.At(directInputList.countRva, sizeof(int));
    if (!devices || !count) return false;

    g_nativeDevices = reinterpret_cast<IUnknown**>(
        const_cast<std::byte*>(devices->data()));
    g_nativeDeviceCount = reinterpret_cast<int*>(
        const_cast<std::byte*>(count->data()));

    const auto target = image.At(contract.entryRva, expected.size());
    return target && g_scanDetour.Install(
        "event-driven-controller-hotplug",
        const_cast<std::byte*>(target->data()),
        reinterpret_cast<void*>(&DeviceListScanHook), expected,
        reinterpret_cast<void**>(&g_originalScan));
}

} // namespace

bool Initialize() {
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    g_contract = ContractFor(CurrentProfile().title);
    if (!image || !g_contract) {
        LogWarning("Smart controller hotplug disabled: no native title contract");
        return false;
    }

    std::array<std::byte, 16> expectedStorage{};
    if (g_contract->entryPrefix.size() > expectedStorage.size()) {
        LogWarning("Smart controller hotplug disabled: invalid entry contract size");
        return false;
    }
    std::memcpy(expectedStorage.data(), g_contract->entryPrefix.data(),
                g_contract->entryPrefix.size());
    std::span<std::byte> expectedMutable{
        expectedStorage.data(), g_contract->entryPrefix.size()};
    if (g_contract->directInputList &&
        !ValidateCountOperand(*image, *g_contract,
                              *g_contract->directInputList,
                              expectedMutable)) {
        LogWarning(
            "Smart controller hotplug disabled: title=%s device-count operand mismatch",
            g_contract->titleName);
        return false;
    }

    const std::span<const std::byte> expected = expectedMutable;
    const auto entry = image->At(g_contract->entryRva, expected.size());
    if (!entry ||
        std::memcmp(entry->data(), expected.data(), expected.size()) != 0) {
        LogWarning(
            "Smart controller hotplug disabled: title=%s entry contract mismatch",
            g_contract->titleName);
        return false;
    }

    bool installed = false;
    if (g_contract->strategy == Strategy::ValidatedDirectInputList) {
        installed = InitializeDirectInputList(*image, *g_contract, expected);
        g_gate.Reset();
    } else {
        g_nativeRescanRequest = reinterpret_cast<NativeRescanRequestFn>(
            const_cast<std::byte*>(entry->data()));
        installed = true;
    }

    if (!installed) {
        LogWarning(
            "Smart controller hotplug disabled: title=%s native backend install failed",
            g_contract->titleName);
        return false;
    }

    g_available.store(true, std::memory_order_release);
    return true;
}

void NotifyTopologyChanged() {
    if (!g_available.load(std::memory_order_acquire)) return;
    if (g_contract->strategy == Strategy::ValidatedDirectInputList) {
        g_rebuildRequested.store(true, std::memory_order_release);
        g_gate.Notify();
    } else {
        g_nativeRequestPending.store(true, std::memory_order_release);
    }
}

void Pump() {
    if (!g_available.load(std::memory_order_acquire) ||
        !settings::ActivePerformance().smartControllerHotplug ||
        g_contract->strategy != Strategy::NativeAsyncRequest ||
        !g_nativeRequestPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    g_nativeRescanRequest();
    Log("Controller rescan requested: title=%s", g_contract->titleName);
}

bool RequiresXInputVerification() {
    return g_contract &&
           g_contract->strategy == Strategy::ValidatedDirectInputList;
}

} // namespace novafix::game::controller_hotplug::backend
