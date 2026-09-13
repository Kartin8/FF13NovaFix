#include "game/titles/ff13_2/camera/scene_a_bahamut.h"

#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/shared/timing/frame_pacing.h"
#include "patch/relative_branch.h"
#include "patch/transaction.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace novafix::game::xiii2_scene_a_bahamut {
namespace {

constexpr std::uint32_t kAcosContextRva = 0x00129C42u;
constexpr std::uint32_t kAcosCallRva = 0x00129C4Eu;
constexpr std::uint32_t kNativeAcosRva = 0x008EB7F6u;
constexpr std::size_t kCallOffset = kAcosCallRva - kAcosContextRva;
constexpr std::size_t kStubSize = 56u;
constexpr std::size_t kStubJumpOffset = 31u;
constexpr std::size_t kUpperBoundOffset = 40u;
constexpr std::size_t kLowerBoundOffset = 48u;

constexpr std::array<std::uint8_t, 24> kAcosContext{
    0xF3, 0x0F, 0x10, 0x84, 0x24, 0x20, 0x02, 0x00,
    0x00, 0x0F, 0x5A, 0xC0, 0xE8, 0xA3, 0x1B, 0x7C,
    0x00, 0x0F, 0x57, 0xD2, 0xF2, 0x0F, 0x5A, 0xC0};
constexpr std::array<std::uint8_t, 5> kAcosCall{
    0xE8, 0xA3, 0x1B, 0x7C, 0x00};

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_installed{false};
volatile LONG g_enabled{};

bool StoreAddress(std::array<std::byte, kStubSize>& stub,
                  std::size_t offset, const void* address) {
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);
    if (value > (std::numeric_limits<std::uint32_t>::max)() ||
        offset > stub.size() - sizeof(std::uint32_t)) {
        return false;
    }
    const std::uint32_t address32 = static_cast<std::uint32_t>(value);
    std::memcpy(stub.data() + offset, &address32, sizeof(address32));
    return true;
}

bool StoreRelativeJump(std::array<std::byte, kStubSize>& stub,
                       std::byte* memory, const void* destination) {
    const std::uintptr_t instruction =
        reinterpret_cast<std::uintptr_t>(memory + kStubJumpOffset);
    const std::uintptr_t target =
        reinterpret_cast<std::uintptr_t>(destination);
    const std::int64_t displacement = static_cast<std::int64_t>(target) -
        static_cast<std::int64_t>(instruction + 5u);
    if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
        displacement > (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    const std::int32_t relative = static_cast<std::int32_t>(displacement);
    std::memcpy(stub.data() + kStubJumpOffset + 1u, &relative,
                sizeof(relative));
    return true;
}

bool PopulateStub(std::byte* memory, const void* nativeAcos) {
    // Unrelated NaNs keep their native behavior; only ordered values are
    // constrained to acos's mathematical domain before the tail call
    std::array<std::byte, kStubSize> stub{
        std::byte{0x83}, std::byte{0x3D}, std::byte{}, std::byte{},
        std::byte{}, std::byte{}, std::byte{}, std::byte{0x74},
        std::byte{0x16}, std::byte{0x66}, std::byte{0x0F},
        std::byte{0x2E}, std::byte{0xC0}, std::byte{0x7A},
        std::byte{0x10}, std::byte{0xF2}, std::byte{0x0F},
        std::byte{0x5D}, std::byte{0x05}, std::byte{}, std::byte{},
        std::byte{}, std::byte{}, std::byte{0xF2}, std::byte{0x0F},
        std::byte{0x5F}, std::byte{0x05}, std::byte{}, std::byte{},
        std::byte{}, std::byte{}, std::byte{0xE9}, std::byte{},
        std::byte{}, std::byte{}, std::byte{}, std::byte{0x90},
        std::byte{0x90}, std::byte{0x90}, std::byte{0x90}};

    if (!StoreAddress(stub, 2u,
                      const_cast<const LONG*>(&g_enabled)) ||
        !StoreAddress(stub, 19u, memory + kUpperBoundOffset) ||
        !StoreAddress(stub, 27u, memory + kLowerBoundOffset) ||
        !StoreRelativeJump(stub, memory, nativeAcos)) {
        return false;
    }
    constexpr double upper = 1.0;
    constexpr double lower = -1.0;
    std::memcpy(stub.data() + kUpperBoundOffset, &upper, sizeof(upper));
    std::memcpy(stub.data() + kLowerBoundOffset, &lower, sizeof(lower));
    std::memcpy(memory, stub.data(), stub.size());

    DWORD oldProtection{};
    if (!VirtualProtect(memory, stub.size(), PAGE_EXECUTE_READ,
                        &oldProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), memory, stub.size())) {
        return false;
    }
    return true;
}

void* CreateStub(const void* nativeAcos) {
    auto* memory = static_cast<std::byte*>(VirtualAlloc(
        nullptr, kStubSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return nullptr;
    if (PopulateStub(memory, nativeAcos)) return memory;
    VirtualFree(memory, 0u, MEM_RELEASE);
    return nullptr;
}

BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::FramePacing)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    const auto context = image
        ? image->At(kAcosContextRva, kAcosContext.size())
        : std::nullopt;
    const auto nativeAcos = image ? image->At(kNativeAcosRva, 1u)
                                  : std::nullopt;
    if (!image || !context || !nativeAcos ||
        std::memcmp(context->data(), kAcosContext.data(),
                    kAcosContext.size()) != 0) {
        LogWarning("XIII-2 Scene A camera correction disabled: executable contract mismatch");
        return TRUE;
    }

    std::byte* stub = static_cast<std::byte*>(CreateStub(
        nativeAcos->data()));
    if (!stub) {
        LogWarning("XIII-2 Scene A camera correction disabled: clamp stub creation failed");
        return TRUE;
    }
    const auto replacement = patch::EncodeRelativeCall32(
        reinterpret_cast<std::uintptr_t>(context->data() + kCallOffset),
        reinterpret_cast<std::uintptr_t>(stub));
    if (!replacement) {
        VirtualFree(stub, 0u, MEM_RELEASE);
        LogWarning("XIII-2 Scene A camera correction disabled: clamp stub is outside call range");
        return TRUE;
    }

    std::byte* call = const_cast<std::byte*>(
        context->data() + kCallOffset);
    patch::Transaction transaction(
        "ff13-2-scene-a-bahamut-acos", std::span<std::byte>(call, 5u),
        patch::registry::Kind::Detour);
    if (!transaction.Add(
            0u, std::as_bytes(std::span{kAcosCall}), *replacement)) {
        VirtualFree(stub, 0u, MEM_RELEASE);
        LogWarning("XIII-2 Scene A camera correction disabled: invalid call redirect");
        return TRUE;
    }
    const patch::ApplyStatus status = transaction.Apply();
    if (status != patch::ApplyStatus::Applied &&
        status != patch::ApplyStatus::AlreadyApplied) {
        VirtualFree(stub, 0u, MEM_RELEASE);
        LogWarning("XIII-2 Scene A camera correction disabled: patch status=%s",
                   patch::ApplyStatusName(status));
        return TRUE;
    }

    g_installed.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &InitializeOnce, nullptr, nullptr);
}

void Pump() {
    if (!g_installed.load(std::memory_order_acquire)) return;
    const settings::PerformanceSettings profile =
        settings::ActivePerformance();
    const bool enabled = profile.highFpsCorrectness &&
        (profile.frameRateLimit == 0u || profile.frameRateLimit > 30u) &&
        frame_pacing::IsHighRefreshPresentationActive();
    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
}

} // namespace novafix::game::xiii2_scene_a_bahamut
