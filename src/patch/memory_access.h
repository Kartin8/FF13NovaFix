#pragma once

#include <cstddef>
#include <span>

namespace novafix::patch::memory {

struct WriteResult {
    bool bytesWritten{};
    bool instructionCacheFlushed{};
    bool protectionsRestored{true};

    bool Succeeded() const {
        return bytesWritten && instructionCacheFlushed && protectionsRestored;
    }
};

// Validate every memory region crossed by the range
bool IsReadable(const void* address, std::size_t size);
bool IsWritable(const void* address, std::size_t size);
bool IsExecutable(const void* address, std::size_t size);

// Count readable bytes up to maximumSize without re-querying regions
std::size_t ReadablePrefixSize(const void* address, std::size_t maximumSize);

// Restore the original protection for every region touched by the write
WriteResult Write(std::byte* address, std::span<const std::byte> bytes);

} // namespace novafix::patch::memory
