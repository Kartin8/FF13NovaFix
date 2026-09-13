#include "game/titles/ff13_2/resources/unwrap_policy.h"

#include "common/guarded_call.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace novafix::game::xiii2_resource_unwrap {
namespace {

constexpr std::uint32_t kWrapperMarker = 0xB00BEED0u;
constexpr std::size_t kMaximumMarkerWords = 32;
constexpr std::size_t kNativeVtableCapacity = 32;
std::array<std::atomic<void*>, kNativeVtableCapacity> g_nativeVtables{};

bool HasExecutableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;

    switch (protection & 0xFFu) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsExecutableAddress(const void* address) noexcept {
    constexpr std::size_t size = 1u;
    if (!address) return false;

    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (start > UINTPTR_MAX - size) return false;

    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT) {
        return false;
    }

    const auto regionStart = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (regionStart > UINTPTR_MAX - memory.RegionSize) return false;
    const auto regionEnd = regionStart + memory.RegionSize;
    if (start + size > regionEnd) return false;

    return HasExecutableProtection(memory.Protect);
}

bool TryReadPointer(const void* address, void*& value) noexcept {
    if (!address) return false;
#if defined(_MSC_VER)
    __try {
        value = *static_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = nullptr;
        return false;
    }
#else
    if (guarded_call::Run([&] {
        value = *static_cast<void* const*>(address);
    })) return true;
    value = nullptr;
    return false;
#endif
}

bool TryReadWord(const void* address, std::uint32_t& value) noexcept {
    if (!address) return false;
#if defined(_MSC_VER)
    __try {
        value = *static_cast<const std::uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
#else
    if (guarded_call::Run([&] {
        value = *static_cast<const std::uint32_t*>(address);
    })) return true;
    value = 0;
    return false;
#endif
}

bool IsInsideImage(const void* address, ImageRange image) noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(image.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(image.end);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return begin != 0 && end > begin && value >= begin && value < end;
}

bool IsPlausibleVtable(void* vtable) noexcept {
    if (!vtable) return false;
    for (const auto& known : g_nativeVtables) {
        if (known.load(std::memory_order_acquire) == vtable) return true;
    }

    void* firstMethod = nullptr;
    if (!TryReadPointer(vtable, firstMethod) || !firstMethod ||
        !IsExecutableAddress(firstMethod)) {
        return false;
    }

    for (auto& known : g_nativeVtables) {
        void* empty = nullptr;
        if (known.compare_exchange_strong(
                empty, vtable, std::memory_order_release,
                std::memory_order_relaxed) || empty == vtable) {
            break;
        }
    }
    return true;
}

bool IsPlausibleComObject(void* object) noexcept {
    void* vtable = nullptr;
    return TryReadPointer(object, vtable) && IsPlausibleVtable(vtable);
}

Resolution ResolveWrappedResource(void* candidate) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(candidate);

    for (std::size_t index = 0; index < kMaximumMarkerWords; ++index) {
        const std::size_t byteOffset = index * sizeof(std::uint32_t);
        if (base > UINTPTR_MAX - byteOffset) break;

        std::uint32_t word = 0;
        if (!TryReadWord(reinterpret_cast<const void*>(base + byteOffset), word)) {
            break;
        }
        if (word != kWrapperMarker) continue;

        // The payload follows the marker. The marker is allowed in the final
        // scan slot, so this intentionally validates word 32 as well
        const std::size_t payloadOffset = byteOffset + sizeof(std::uint32_t);
        if (base > UINTPTR_MAX - payloadOffset) break;

        void* payload = nullptr;
        if (!TryReadPointer(reinterpret_cast<const void*>(base + payloadOffset), payload) ||
            !IsPlausibleComObject(payload)) {
            return {};
        }
        return {payload, ResolutionKind::Wrapped};
    }

    return {};
}

} // namespace

Resolution ResolveResource(void* candidate, ImageRange gameImage) noexcept {
    if (!candidate) return {};

    void* vtable = nullptr;
    if (!TryReadPointer(candidate, vtable) || !vtable) return {};

    // Native D3D9, DXVK and third-party proxy objects keep their vtables
    // outside the game image. This is the common path and avoids the wrapper
    // scan that caused the original out-of-bounds read
    if (!IsInsideImage(vtable, gameImage)) {
        return IsPlausibleVtable(vtable)
                   ? Resolution{candidate, ResolutionKind::Native}
                   : Resolution{};
    }

    // Crystal Tools wrappers use a game-owned vtable and store the real COM
    // resource immediately after a marker somewhere in the first 32 words
    return ResolveWrappedResource(candidate);
}

} // namespace novafix::game::xiii2_resource_unwrap
