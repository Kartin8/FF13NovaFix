#include "game/titles/ff13_2/resources/unwrap.h"

#include "diagnostics/hook_profiler.h"
#include "diagnostics/log.h"
#include "game/core/game_profile.h"
#include "game/core/image_view.h"
#include "game/titles/ff13_2/resources/unwrap_policy.h"
#include "patch/detour32.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_resource_unwrap {
namespace {

// XIII-2 Steam 2014: this helper accepts either a Crystal Tools wrapper or a
// native D3D9 COM resource and must return the underlying COM interface
constexpr std::uint32_t kResourceUnwrapRva = 0x0082FA30u;
constexpr std::uint32_t kWrapperModeFlagRva = 0x01FA8644u;

constexpr std::size_t kResourceUnwrapPrefixSize = 7;

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
patch::Detour32 g_detour;
ImageRange g_gameImage{};
std::atomic_uint32_t g_rejectedCalls{0};

bool HasExpectedEntryContract(const std::byte* target,
                              const void* wrapperModeFlag) noexcept {
    if (target[0] != std::byte{0x80} ||
        target[1] != std::byte{0x3D} ||
        target[6] != std::byte{0x00}) {
        return false;
    }

    std::uint32_t encodedAddress = 0;
    std::memcpy(&encodedAddress, target + 2, sizeof(encodedAddress));
    return encodedAddress == static_cast<std::uint32_t>(
                                 reinterpret_cast<std::uintptr_t>(
                                     wrapperModeFlag));
}

void* __cdecl ResourceUnwrapHook(void* resource) {
    NOVAFIX_PROFILE_SAMPLED_NAMED_SCOPE(
        hookTiming, "hook.ff13-2.resource-unwrap-total", 64u);
    const Resolution resolution = ResolveResource(resource, g_gameImage);
    if (resolution.kind == ResolutionKind::Rejected) {
        const std::uint32_t count =
            g_rejectedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1) {
            Log("XIII-2 resource unwrap rejected an invalid object at %p", resource);
        }
    }
    return resolution.resource;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2 ||
        !Supports(Capability::MemorySafety)) {
        return TRUE;
    }

    const auto image = ImageView::FromModule(GetModuleHandleW(nullptr));
    if (!image) {
        LogWarning("XIII-2 resource unwrap disabled: main image unavailable");
        return TRUE;
    }

    const auto bytes = image->Bytes();
    g_gameImage = {bytes.data(), bytes.data() + bytes.size()};

    const auto targetRange = image->At(
        kResourceUnwrapRva, kResourceUnwrapPrefixSize);
    const auto flagRange = image->At(kWrapperModeFlagRva, sizeof(std::byte));
    const auto flagAddress = image->Address32(
        kWrapperModeFlagRva, sizeof(std::byte));
    if (!targetRange || !flagRange || !flagAddress) {
        LogWarning("XIII-2 resource unwrap disabled: image is smaller than its RVA contract");
        return TRUE;
    }

    std::byte* target = const_cast<std::byte*>(targetRange->data());
    const void* wrapperModeFlag = flagRange->data();
    if (!HasExpectedEntryContract(target, wrapperModeFlag)) {
        LogWarning("XIII-2 resource unwrap disabled: byte contract mismatch at RVA 0x%X",
            kResourceUnwrapRva);
        return TRUE;
    }

    std::array<std::byte, kResourceUnwrapPrefixSize> livePrefix{};
    std::memcpy(livePrefix.data(), target, livePrefix.size());
    if (!g_detour.Install(
            "ff13-2-resource-unwrap", target,
            reinterpret_cast<void*>(&ResourceUnwrapHook),
            livePrefix, nullptr)) {
        LogWarning("XIII-2 resource unwrap disabled: detour ownership failed");
        return TRUE;
    }

    Log("XIII-2 resource unwrap: RVA=0x%X", kResourceUnwrapRva);
    return TRUE;
}

} // namespace

void Pump() {
    InitOnceExecuteOnce(&g_initializeOnce, &Initialize, nullptr, nullptr);
}

} // namespace novafix::game::xiii2_resource_unwrap
