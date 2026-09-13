#include "patch/memory_access.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace novafix::patch::memory {
namespace {

struct Region {
    std::byte* address{};
    std::size_t size{};
    DWORD protection{};
};

bool ValidRange(const void* address, std::size_t size,
                std::uintptr_t* beginOut, std::uintptr_t* endOut) {
    if (!address || size == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin > (std::numeric_limits<std::uintptr_t>::max)() - size) {
        return false;
    }
    if (beginOut) *beginOut = begin;
    if (endOut) *endOut = begin + size;
    return true;
}

bool IsReadableProtection(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xFFu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsExecutableProtection(DWORD protection) {
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

bool IsWritableProtection(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xFFu) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

template <typename Predicate>
bool HasProtection(const void* address, std::size_t size,
                   Predicate predicate) {
    std::uintptr_t current{};
    std::uintptr_t end{};
    if (!ValidRange(address, size, &current, &end)) return false;

    while (current < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &information,
                         sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT || !predicate(information.Protect)) {
            return false;
        }
        const auto regionBegin =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        if (regionBegin >
            (std::numeric_limits<std::uintptr_t>::max)() - information.RegionSize) {
            return false;
        }
        const std::uintptr_t regionEnd = regionBegin +
            static_cast<std::uintptr_t>(information.RegionSize);
        if (current < regionBegin || current >= regionEnd) return false;
        current = (std::min)(end, regionEnd);
    }
    return true;
}

bool CollectRegions(std::byte* address, std::size_t size,
                    std::vector<Region>* regions) {
    std::uintptr_t current{};
    std::uintptr_t end{};
    if (!regions || !ValidRange(address, size, &current, &end)) return false;

    while (current < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &information,
                         sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        const auto regionBegin =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        if (regionBegin >
            (std::numeric_limits<std::uintptr_t>::max)() - information.RegionSize) {
            return false;
        }
        const std::uintptr_t regionEnd = regionBegin +
            static_cast<std::uintptr_t>(information.RegionSize);
        if (current < regionBegin || current >= regionEnd) return false;
        const auto segmentEnd = (std::min)(end, regionEnd);
        regions->push_back({
            reinterpret_cast<std::byte*>(current),
            static_cast<std::size_t>(segmentEnd - current),
            information.Protect,
        });
        current = segmentEnd;
    }
    return !regions->empty();
}

bool RestoreProtections(std::span<const Region> regions) {
    bool restored = true;
    for (std::size_t count = regions.size(); count > 0; --count) {
        const Region& region = regions[count - 1];
        DWORD ignored{};
        restored = VirtualProtect(region.address, region.size,
                                  region.protection, &ignored) != FALSE &&
                   restored;
    }
    return restored;
}

} // namespace

bool IsReadable(const void* address, std::size_t size) {
    return HasProtection(address, size, &IsReadableProtection);
}

bool IsWritable(const void* address, std::size_t size) {
    return HasProtection(address, size, &IsWritableProtection);
}

bool IsExecutable(const void* address, std::size_t size) {
    return HasProtection(address, size, &IsExecutableProtection);
}

std::size_t ReadablePrefixSize(const void* address,
                               std::size_t maximumSize) {
    std::uintptr_t current{};
    std::uintptr_t end{};
    if (!ValidRange(address, maximumSize, &current, &end)) return 0u;

    const std::uintptr_t begin = current;
    while (current < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &information,
                         sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT ||
            !IsReadableProtection(information.Protect)) {
            break;
        }
        const auto regionBegin =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        if (regionBegin >
            (std::numeric_limits<std::uintptr_t>::max)() -
                information.RegionSize) {
            break;
        }
        const std::uintptr_t regionEnd = regionBegin +
            static_cast<std::uintptr_t>(information.RegionSize);
        if (current < regionBegin || current >= regionEnd) break;
        current = (std::min)(end, regionEnd);
    }
    return static_cast<std::size_t>(current - begin);
}

WriteResult Write(std::byte* address, std::span<const std::byte> bytes) {
    WriteResult result{};
    std::vector<Region> regions;
    if (bytes.empty() || !CollectRegions(address, bytes.size(), &regions)) {
        return result;
    }

    std::size_t changed = 0;
    for (; changed < regions.size(); ++changed) {
        DWORD oldProtection{};
        Region& region = regions[changed];
        if (!VirtualProtect(region.address, region.size,
                            PAGE_EXECUTE_READWRITE, &oldProtection)) {
            result.protectionsRestored =
                RestoreProtections(std::span(regions).first(changed));
            return result;
        }
        region.protection = oldProtection;
    }

    std::memcpy(address, bytes.data(), bytes.size());
    result.bytesWritten = true;
    result.instructionCacheFlushed =
        FlushInstructionCache(GetCurrentProcess(), address, bytes.size()) != FALSE;
    result.protectionsRestored = RestoreProtections(regions);
    return result;
}

} // namespace novafix::patch::memory
