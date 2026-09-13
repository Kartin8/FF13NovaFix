#include "game/titles/ff13_2/resources/budget.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "patch/detour32.h"
#include "performance/frame_metrics.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_resource_budget {
namespace {

constexpr std::uint32_t kBudgetGetterRva = 0x00035550u;
constexpr std::uint32_t kBudgetStorageRva = 0x0211E370u;
constexpr std::uint32_t kModelBudgetReturnRva = 0x0003334Au;
constexpr std::size_t kBudgetGetterSize = 6u;
constexpr unsigned kAuthoredRate = 30u;
constexpr std::uint32_t kMinimumProgressUs = 1'000u;

std::uint32_t ScaleModelBudget(std::uint32_t originalBudgetUs,
                               unsigned targetRate) noexcept {
    if (originalBudgetUs == 0u || targetRate <= kAuthoredRate) {
        return originalBudgetUs;
    }

    const std::uint64_t scaled =
        static_cast<std::uint64_t>(originalBudgetUs) * kAuthoredRate /
        targetRate;
    return std::min(
        originalBudgetUs,
        std::max(kMinimumProgressUs, static_cast<std::uint32_t>(scaled)));
}

using BudgetGetterFn = std::uint32_t(__cdecl*)();

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_budgetGetterDetour;
BudgetGetterFn g_originalBudgetGetter{};
const std::byte* g_imageBase{};
std::atomic_uint g_targetRate{0u};
std::atomic_bool g_installed{false};
std::atomic_uint32_t g_lastLoggedBudget{0u};

bool HasExpectedGetterContract(const std::byte* target,
                               const void* budgetStorage) noexcept {
    if (target[0] != std::byte{0xA1} ||
        target[5] != std::byte{0xC3}) {
        return false;
    }

    std::uint32_t encodedAddress = 0u;
    std::memcpy(&encodedAddress, target + 1, sizeof(encodedAddress));
    return encodedAddress == static_cast<std::uint32_t>(
                                 reinterpret_cast<std::uintptr_t>(
                                     budgetStorage));
}

std::uint32_t CallerRva(const void* caller) noexcept {
    if (!caller || !g_imageBase) return 0u;
    const auto address = reinterpret_cast<std::uintptr_t>(caller);
    const auto base = reinterpret_cast<std::uintptr_t>(g_imageBase);
    if (address < base || address - base > UINT32_MAX) return 0u;
    return static_cast<std::uint32_t>(address - base);
}

std::uint32_t __cdecl BudgetGetterHook() {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.resource-budget-total", 64u);
    BudgetGetterFn original = g_originalBudgetGetter;
    if (!original) return 0u;

    const void* caller = _ReturnAddress();
    const std::uint32_t originalBudget = original();
    if (CallerRva(caller) != kModelBudgetReturnRva) {
        return originalBudget;
    }

    return ScaleModelBudget(
        originalBudget, g_targetRate.load(std::memory_order_relaxed));
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::FramePacing)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII-2 resource budget correction disabled: main image unavailable");
        return TRUE;
    }

    const auto bytes = image->Bytes();
    const auto getter = image->At(kBudgetGetterRva, kBudgetGetterSize);
    const auto storage = image->At(
        kBudgetStorageRva, sizeof(std::uint32_t));
    const auto storageAddress = image->Address32(
        kBudgetStorageRva, sizeof(std::uint32_t));
    if (!getter || !storage || !storageAddress) {
        LogWarning("XIII-2 resource budget correction disabled: image is smaller than its RVA contract");
        return TRUE;
    }

    std::byte* target = const_cast<std::byte*>(getter->data());
    const void* budgetStorage = storage->data();
    if (!HasExpectedGetterContract(target, budgetStorage)) {
        LogWarning("XIII-2 resource budget correction disabled: byte contract mismatch at RVA 0x%X",
            kBudgetGetterRva);
        return TRUE;
    }

    std::array<std::byte, kBudgetGetterSize> livePrefix{};
    std::memcpy(livePrefix.data(), target, livePrefix.size());
    g_imageBase = bytes.data();
    if (!g_budgetGetterDetour.Install(
            "ff13-2-resource-budget", target,
            reinterpret_cast<void*>(&BudgetGetterHook),
            livePrefix,
            reinterpret_cast<void**>(&g_originalBudgetGetter))) {
        g_imageBase = nullptr;
        LogWarning("XIII-2 resource budget correction disabled: detour ownership failed");
        return TRUE;
    }

    g_installed.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Initialize() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

void Pump() {
    if (!g_installed.load(std::memory_order_acquire)) return;

    const unsigned targetRate = performance::CurrentTargetRate();
    if (g_targetRate.load(std::memory_order_relaxed) != targetRate) {
        g_targetRate.store(targetRate, std::memory_order_relaxed);
    }

    const std::uint32_t scaled = ScaleModelBudget(15'000u, targetRate);
    std::uint32_t logged = g_lastLoggedBudget.load(std::memory_order_relaxed);
    if (scaled != logged &&
        g_lastLoggedBudget.compare_exchange_strong(
            logged, scaled, std::memory_order_relaxed)) {
        Log("XIII-2 resource budget: target=%u Hz model-slice=%u us",
            targetRate, scaled);
    }
}

} // namespace novafix::game::xiii2_resource_budget
