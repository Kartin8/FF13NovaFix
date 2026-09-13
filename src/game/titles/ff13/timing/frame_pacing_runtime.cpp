#include "game/titles/ff13/timing/frame_pacing_runtime.h"

#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/shared/timing/frame_pacing_state.h"
#include "patch/memory_access.h"
#include "patch/transaction.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace novafix::game::ff13_frame_pacing_runtime {
namespace {

constexpr float kDisabledGamePacer = 250000.0f;
constexpr std::uint32_t kStatePointerRva = 0x243E34Cu;
constexpr std::uint32_t kSetterRva = 0xA8D65Fu;
constexpr std::array<std::uint8_t, 5> kSetterBytes{
    0xF3, 0x0F, 0x11, 0x40, 0x04};
constexpr std::array<std::uint8_t, 5> kNops{
    0x90, 0x90, 0x90, 0x90, 0x90};

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_supported{};
float** g_frameStatePointer{};
float* g_frameState{};
std::unique_ptr<patch::Transaction> g_setterPatch;
SRWLOCK g_stateLock = SRWLOCK_INIT;

void WriteFloat(float* address, float value) {
    const LONG bits = std::bit_cast<LONG>(value);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(address), bits);
}

bool SameFloat(float left, float right) {
    return std::bit_cast<std::uint32_t>(left) ==
        std::bit_cast<std::uint32_t>(right);
}

bool InstallSetterPatch(std::byte* setter) {
    g_setterPatch = std::make_unique<patch::Transaction>(
        "disable-game-frame-pacer",
        std::span<std::byte>(setter, kSetterBytes.size()));
    if (!g_setterPatch->Add(0u, std::as_bytes(std::span{kSetterBytes}),
                            std::as_bytes(std::span{kNops}))) {
        g_setterPatch.reset();
        return false;
    }
    return true;
}

BOOL CALLBACK InitializeImpl(PINIT_ONCE, PVOID, PVOID*) {
    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (CurrentProfile().title != Title::FinalFantasyXIII || !image ||
        !Supports(Capability::FramePacing)) {
        LogWarning("Frame pacing fix disabled: unsupported executable or no title contract");
        return TRUE;
    }

    const auto setterRange = image->At(kSetterRva, kSetterBytes.size());
    const auto statePointerRange = image->At(
        kStatePointerRva, sizeof(float*));
    if (!setterRange || !statePointerRange ||
        std::memcmp(setterRange->data(), kSetterBytes.data(),
                    kSetterBytes.size()) != 0) {
        LogWarning("Frame pacing fix disabled: XIII executable contract mismatch");
        return TRUE;
    }

    std::byte* const setter =
        const_cast<std::byte*>(setterRange->data());
    if (!InstallSetterPatch(setter)) {
        LogWarning("Frame pacing fix disabled: XIII setter ownership failed");
        return TRUE;
    }

    g_frameStatePointer = reinterpret_cast<float**>(
        const_cast<std::byte*>(statePointerRange->data()));
    g_supported.store(true, std::memory_order_release);
    return TRUE;
}

bool Activate() {
    if (!g_supported.load(std::memory_order_acquire) ||
        !g_frameStatePointer) {
        return false;
    }
    float* state = *g_frameStatePointer;
    if (!patch::memory::IsWritable(state, sizeof(float) * 2u)) return false;

    if (g_setterPatch && !g_setterPatch->IsApplied()) {
        const patch::ApplyStatus status = g_setterPatch->Apply();
        if (status != patch::ApplyStatus::Applied &&
            status != patch::ApplyStatus::AlreadyApplied) {
            LogWarning("Frame pacing fix disabled: setter patch status=%s",
                patch::ApplyStatusName(status));
            g_supported.store(false, std::memory_order_release);
            return false;
        }
    }

    g_frameState = state;
    WriteFloat(state, kDisabledGamePacer);
    WriteFloat(state + 1, kDisabledGamePacer);
    frame_pacing::state::ownershipActive.store(
        true, std::memory_order_release);
    return true;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(
        &g_initializeOnce, &InitializeImpl, nullptr, nullptr);
}

void Pump() {
    if (!g_supported.load(std::memory_order_acquire)) return;

    AcquireSRWLockExclusive(&g_stateLock);
    if (!frame_pacing::state::ownershipActive.load(
            std::memory_order_acquire)) {
        Activate();
        ReleaseSRWLockExclusive(&g_stateLock);
        return;
    }

    float* replacement = g_frameStatePointer
        ? *g_frameStatePointer : nullptr;
    if (replacement != g_frameState) {
        if (!patch::memory::IsWritable(
                replacement, sizeof(float) * 2u)) {
            g_frameState = nullptr;
            frame_pacing::state::ownershipActive.store(
                false, std::memory_order_release);
            Log("Smooth frame pacing is waiting for a recreated game timing object");
            ReleaseSRWLockExclusive(&g_stateLock);
            return;
        }
        g_frameState = replacement;
        Log("Smooth frame pacing rebound to a recreated game timing object");
    }

    if (!SameFloat(g_frameState[0], kDisabledGamePacer)) {
        WriteFloat(g_frameState, kDisabledGamePacer);
    }
    if (!SameFloat(g_frameState[1], kDisabledGamePacer)) {
        WriteFloat(g_frameState + 1, kDisabledGamePacer);
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

} // namespace novafix::game::ff13_frame_pacing_runtime
