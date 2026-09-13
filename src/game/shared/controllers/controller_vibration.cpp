#include "game/shared/controllers/controller_vibration.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "input/vibration_envelope.h"
#include "input/xinput_backend.h"
#include "patch/detour32.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"
#include "settings/input_settings.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

namespace novafix::game::controller_vibration {
namespace {

// The Windows backend returns false; the input caller then clears both motor amplitudes
constexpr std::array<std::uint8_t, 7> kFf13BackendPrefix{
    0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC,
};
constexpr std::array<std::uint8_t, 5> kFf13_2BackendPrefix{
    0x32, 0xC0, 0xC2, 0x08, 0x00,
};
constexpr std::array<std::uint8_t, 5> kFf13_2VibrationCall{
    0xE8, 0x56, 0xEE, 0x3E, 0x00,
};
constexpr std::array<std::uint8_t, 5> kLrBackendPrefix{
    0x32, 0xC0, 0xC2, 0x08, 0x00,
};
constexpr std::array<std::uint8_t, 5> kLrVibrationCall{
    0xE8, 0x16, 0x08, 0x00, 0x00,
};

struct VibrationCallSite {
    std::uint32_t rva;
    std::span<const std::uint8_t> expected;
};

struct VibrationContract {
    std::uint32_t backendRva;
    std::span<const std::uint8_t> backendPrefix;
    std::optional<VibrationCallSite> callSite;
    const char* title;
};

std::optional<VibrationContract> ContractFor(Title title) {
    switch (title) {
    case Title::FinalFantasyXIII:
        return VibrationContract{
            0x423400u, kFf13BackendPrefix, std::nullopt, "XIII"};
    case Title::FinalFantasyXIII2:
        return VibrationContract{
            0x696070u, kFf13_2BackendPrefix,
            VibrationCallSite{0x2A7215u, kFf13_2VibrationCall},
            "XIII-2"};
    case Title::LightningReturns:
        return VibrationContract{
            0x344D40u, kLrBackendPrefix,
            VibrationCallSite{0x344525u, kLrVibrationCall},
            "LR"};
    case Title::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}
constexpr ULONGLONG kSlowOutputThresholdMs = 8u;
constexpr ULONGLONG kSlowOutputLogIntervalMs = 30000u;

std::uint16_t MotorSpeed(float amplitude, unsigned strengthPercent) {
    if (!std::isfinite(amplitude) || amplitude <= 0.0f ||
        strengthPercent == 0u) {
        return 0u;
    }
    const float scale = static_cast<float>(strengthPercent) / 100.0f;
    const float normalized = std::clamp(amplitude * scale, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(normalized * 65535.0f + 0.5f);
}

using GameVibrationFn = bool (__thiscall*)(void* context, float lowFrequency,
                                           float highFrequency);
INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_backendDetour;
std::unique_ptr<patch::Transaction> g_backendCallPatch;
GameVibrationFn g_originalBackend = nullptr;
std::atomic_bool g_available{false};
std::atomic_uint g_strengthPercent{100};
std::atomic_int g_controllerIndex{-1};
std::atomic_bool g_rescanRequested{true};
std::atomic_bool g_motorActive{false};
std::atomic_uint g_pendingMotorLow{0};
std::atomic_uint g_pendingMotorHigh{0};
SRWLOCK g_nativeEnvelopeLock = SRWLOCK_INIT;
input::VibrationEnvelope g_nativeEnvelope;
std::atomic_bool g_nativeEnvelopeActive{false};
HANDLE g_outputEvent{};
std::atomic_uint g_desiredOutput{};
std::atomic_uint64_t g_outputRevision{};
std::atomic_bool g_forceOutputRefresh{true};
std::atomic_uint64_t g_lastSlowOutputLogAt{};

// These values belong exclusively to the asynchronous output worker
int g_appliedController{-1};
std::uint32_t g_appliedOutput{0xFFFFFFFFu};

void AccumulateMaximum(std::atomic_uint& destination, unsigned value) {
    unsigned current = destination.load(std::memory_order_relaxed);
    while (current < value &&
           !destination.compare_exchange_weak(
               current, value, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

int FindController() {
    const int current = g_controllerIndex.load(std::memory_order_relaxed);
    const bool rescan = g_rescanRequested.exchange(false, std::memory_order_acq_rel);
    // SetState checks liveness without an extra input poll. Rescan only on
    // topology changes or output failure to avoid Bluetooth/HID contention
    if (!rescan && current >= 0) return current;

    for (unsigned index = 0; index < 4; ++index) {
        if (!input::xinput::IsConnected(index)) continue;
        if (current != static_cast<int>(index)) {
            Log("Controller vibration target selected: XInput user %u", index);
        }
        g_controllerIndex.store(static_cast<int>(index), std::memory_order_release);
        return static_cast<int>(index);
    }
    if (current >= 0) Log("Controller vibration target disconnected");
    g_controllerIndex.store(-1, std::memory_order_release);
    return -1;
}

std::uint32_t PackOutput(std::uint16_t lowFrequency,
                         std::uint16_t highFrequency) {
    return static_cast<std::uint32_t>(lowFrequency) |
           (static_cast<std::uint32_t>(highFrequency) << 16u);
}

std::uint16_t LowMotor(std::uint32_t output) {
    return static_cast<std::uint16_t>(output & 0xFFFFu);
}

std::uint16_t HighMotor(std::uint32_t output) {
    return static_cast<std::uint16_t>(output >> 16u);
}

void RequestOutput(std::uint16_t lowFrequency,
                   std::uint16_t highFrequency, bool force = false) {
    const std::uint32_t desired = PackOutput(lowFrequency, highFrequency);
    const std::uint32_t previous = g_desiredOutput.exchange(
        desired, std::memory_order_acq_rel);
    if (!force && previous == desired) return;

    if (force) {
        g_forceOutputRefresh.store(true, std::memory_order_release);
    }
    g_outputRevision.fetch_add(1u, std::memory_order_release);
    if (g_outputEvent) SetEvent(g_outputEvent);
}

bool ApplyXInputOutput(int controller, std::uint32_t output) {
    const std::uint16_t low = LowMotor(output);
    const std::uint16_t high = HighMotor(output);
    const ULONGLONG started = GetTickCount64();
    bool sent = false;
    {
        hook_profiler::Scope profile(
            hook_profiler::Point::ControllerVibrationSetState);
        sent = input::xinput::SetVibration(
            static_cast<unsigned>(controller), low, high);
    }
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed >= kSlowOutputThresholdMs) {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG previous =
            g_lastSlowOutputLogAt.load(std::memory_order_relaxed);
        if (now - previous >= kSlowOutputLogIntervalMs &&
            g_lastSlowOutputLogAt.compare_exchange_strong(
                previous, now, std::memory_order_relaxed)) {
            LogWarning(
                "Slow asynchronous XInput vibration output: duration=%llums controller=%d low=%u high=%u result=%d",
                static_cast<unsigned long long>(elapsed), controller,
                static_cast<unsigned>(low), static_cast<unsigned>(high),
                sent ? 1 : 0);
        }
    }
    return sent;
}

void ApplyDesiredOutput(bool force) {
    const std::uint32_t desired =
        g_desiredOutput.load(std::memory_order_acquire);
    int controller = FindController();
    if (controller < 0) {
        g_appliedController = -1;
        g_appliedOutput = 0xFFFFFFFFu;
        return;
    }
    if (!force && controller == g_appliedController &&
        desired == g_appliedOutput) {
        return;
    }

    bool sent = ApplyXInputOutput(controller, desired);
    if (!sent) {
        g_controllerIndex.store(-1, std::memory_order_release);
        g_rescanRequested.store(true, std::memory_order_release);
        controller = FindController();
        sent = controller >= 0 && ApplyXInputOutput(controller, desired);
    }

    g_appliedController = sent ? controller : -1;
    g_appliedOutput = sent ? desired : 0xFFFFFFFFu;
}

DWORD WINAPI VibrationOutputWorker(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        if (!g_outputEvent ||
            WaitForSingleObject(g_outputEvent, INFINITE) != WAIT_OBJECT_0) {
            return 0u;
        }

        for (;;) {
            const std::uint64_t revision =
                g_outputRevision.load(std::memory_order_acquire);
            const bool force = g_forceOutputRefresh.exchange(
                false, std::memory_order_acq_rel);
            ApplyDesiredOutput(force);
            if (revision ==
                g_outputRevision.load(std::memory_order_acquire)) {
                break;
            }
        }
    }
}

bool StartOutputWorker(DWORD& error) {
    error = ERROR_SUCCESS;
    g_outputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_outputEvent) {
        error = GetLastError();
        return false;
    }
    HANDLE worker = CreateThread(
        nullptr, 0, &VibrationOutputWorker, nullptr, 0, nullptr);
    if (!worker) {
        error = GetLastError();
        CloseHandle(g_outputEvent);
        g_outputEvent = nullptr;
        return false;
    }
    CloseHandle(worker);
    return true;
}

void StopMotors() {
    if (!g_motorActive.load(std::memory_order_acquire) ||
        !g_motorActive.exchange(false, std::memory_order_acq_rel)) return;
    RequestOutput(0, 0);
}

void ClearNativePulse() {
    if (!g_nativeEnvelopeActive.load(std::memory_order_acquire) ||
        !g_nativeEnvelopeActive.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    AcquireSRWLockExclusive(&g_nativeEnvelopeLock);
    g_nativeEnvelope.Reset();
    ReleaseSRWLockExclusive(&g_nativeEnvelopeLock);
}

input::MotorEnvelopeOutput ObserveNativePulse(std::uint16_t low,
                                              std::uint16_t high,
                                              std::uint64_t now) {
    AcquireSRWLockExclusive(&g_nativeEnvelopeLock);
    const input::MotorEnvelopeOutput output = g_nativeEnvelope.Observe(low, high, now);
    ReleaseSRWLockExclusive(&g_nativeEnvelopeLock);
    g_nativeEnvelopeActive.store(output.active, std::memory_order_release);
    return output;
}

input::MotorEnvelopeOutput TickNativePulse(std::uint64_t now) {
    AcquireSRWLockExclusive(&g_nativeEnvelopeLock);
    const input::MotorEnvelopeOutput output = g_nativeEnvelope.Tick(now);
    ReleaseSRWLockExclusive(&g_nativeEnvelopeLock);
    g_nativeEnvelopeActive.store(output.active, std::memory_order_release);
    return output;
}

bool __fastcall VibrationBackendHook(void* context, void*, float lowFrequency,
                                     float highFrequency) {
    thread_local std::uint32_t backendSampleCounter{};
    hook_profiler::Scope backendTiming(
        hook_profiler::Point::ControllerVibrationBackend,
        (backendSampleCounter++ & 63u) == 0u, 64u);
    if (lowFrequency != 0.0f || highFrequency != 0.0f) {
        thread_local std::uint32_t nonZeroSampleCounter{};
        hook_profiler::Scope nonZeroTiming(
            hook_profiler::Point::ControllerVibrationNonZero,
            (nonZeroSampleCounter++ & 63u) == 0u, 64u);
        const unsigned strength =
            g_strengthPercent.load(std::memory_order_relaxed);
        const std::uint16_t low =
            MotorSpeed(lowFrequency, strength);
        const std::uint16_t high =
            MotorSpeed(highFrequency, strength);
        AccumulateMaximum(g_pendingMotorLow, low);
        AccumulateMaximum(g_pendingMotorHigh, high);
    }
    // Retain sub-frame pulse peaks; Pump consumes them once per presentation

    // Return false so the native caller clears its transient amplitudes
    GameVibrationFn original = g_originalBackend;
    return original ? original(context, lowFrequency, highFrequency) : false;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    const auto contract = ContractFor(CurrentProfile().title);
    if (!image || !contract || !Supports(Capability::ControllerVibration)) {
        LogWarning("Controller vibration disabled: unsupported or invalid executable image");
        return TRUE;
    }
    if (!input::xinput::Available()) {
        LogWarning("Controller vibration disabled: no compatible XInput runtime was found");
        return TRUE;
    }

    const auto target = image->At(contract->backendRva,
                                  contract->backendPrefix.size());
    if (!target ||
        std::memcmp(target->data(), contract->backendPrefix.data(),
                    contract->backendPrefix.size()) != 0) {
        LogWarning("Controller vibration disabled: game backend contract mismatch");
        return TRUE;
    }
    const std::byte* address = target->data();
    if (contract->callSite) {
        // XIII-2/LR share this stub with other systems; redirect only the vibration call
        const VibrationCallSite& callSite = *contract->callSite;
        const auto callRange = image->At(callSite.rva, callSite.expected.size());
        if (!callRange ||
            std::memcmp(callRange->data(), callSite.expected.data(),
                        callSite.expected.size()) != 0) {
            LogWarning("Controller vibration disabled: game call-site contract mismatch");
            return TRUE;
        }
        std::byte* call = const_cast<std::byte*>(callRange->data());
        const auto replacementCall = patch::EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(call),
            reinterpret_cast<std::uintptr_t>(&VibrationBackendHook));
        auto callPatch = std::make_unique<patch::Transaction>(
            "native-controller-vibration-call",
            std::span<std::byte>(call, callSite.expected.size()),
            patch::registry::Kind::Detour);
        g_originalBackend = reinterpret_cast<GameVibrationFn>(
            const_cast<std::byte*>(address));
        if (!replacementCall ||
            !callPatch->Add(
                0u, std::as_bytes(callSite.expected), *replacementCall) ||
            callPatch->Apply() != patch::ApplyStatus::Applied) {
            g_originalBackend = nullptr;
            LogWarning("Controller vibration disabled: call-site redirect failed");
            return TRUE;
        }
        g_backendCallPatch = std::move(callPatch);
    } else if (!g_backendDetour.Install(
                   "native-controller-vibration",
                   const_cast<std::byte*>(address),
                   reinterpret_cast<void*>(&VibrationBackendHook),
                   std::as_bytes(contract->backendPrefix),
                   reinterpret_cast<void**>(&g_originalBackend))) {
        LogWarning("Controller vibration disabled: backend detour failed");
        return TRUE;
    }

    DWORD workerError{};
    if (!StartOutputWorker(workerError)) {
        if (g_backendCallPatch) {
            if (g_backendCallPatch->Revert()) {
                g_backendCallPatch.reset();
                g_originalBackend = nullptr;
            } else {
                LogError(
                    "Controller vibration hook rollback failed after output worker creation failure");
            }
        } else {
            if (!g_backendDetour.RevertAndClear(g_originalBackend)) {
                LogError(
                    "Controller vibration detour rollback failed after output worker creation failure");
            }
        }
        LogWarning(
            "Controller vibration disabled: asynchronous XInput output worker creation failed error=%lu",
            workerError);
        return TRUE;
    }

    g_available.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void Pump() {
    if (!g_available.load(std::memory_order_acquire)) return;
    const settings::InputSettings settings = settings::ActiveInput();
    if (g_strengthPercent.load(std::memory_order_relaxed) !=
        settings.vibrationStrengthPercent) {
        g_strengthPercent.store(settings.vibrationStrengthPercent,
                                std::memory_order_relaxed);
    }
    const unsigned observedLow =
        g_pendingMotorLow.load(std::memory_order_relaxed);
    const unsigned observedHigh =
        g_pendingMotorHigh.load(std::memory_order_relaxed);
    const bool envelopeActive =
        g_nativeEnvelopeActive.load(std::memory_order_acquire);
    const bool motorActive = g_motorActive.load(std::memory_order_acquire);
    if (observedLow == 0u && observedHigh == 0u && !envelopeActive &&
        !motorActive) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    const std::uint16_t pendingLow = static_cast<std::uint16_t>(
        observedLow != 0u
            ? g_pendingMotorLow.exchange(0u, std::memory_order_acq_rel)
            : 0u);
    const std::uint16_t pendingHigh = static_cast<std::uint16_t>(
        observedHigh != 0u
            ? g_pendingMotorHigh.exchange(0u, std::memory_order_acq_rel)
            : 0u);
    if (!settings.controllerVibration) {
        ClearNativePulse();
        StopMotors();
    } else {
        const std::uint16_t low = pendingLow;
        const std::uint16_t high = pendingHigh;
        input::MotorEnvelopeOutput native{};
        if (low != 0 || high != 0) {
            native = ObserveNativePulse(low, high, now);
        } else if (envelopeActive) {
            native = TickNativePulse(now);
        }
        if (native.active) {
            RequestOutput(native.low, native.high);
            g_motorActive.store(true, std::memory_order_release);
        } else {
            StopMotors();
        }
    }

}

void NotifyDeviceTopologyChanged() {
    g_rescanRequested.store(true, std::memory_order_release);
    const std::uint32_t desired =
        g_desiredOutput.load(std::memory_order_acquire);
    RequestOutput(LowMotor(desired), HighMotor(desired), true);
}

} // namespace novafix::game::controller_vibration
