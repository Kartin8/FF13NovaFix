#pragma once

#include "patch/memory_access.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace novafix::game {

inline bool CopyBoundedPromptSource(
    const void* source, std::span<std::byte> output,
    std::uint16_t& byteCount) {
    byteCount = 0u;
    if (!source) return false;

    const auto* const bytes = static_cast<const std::byte*>(source);
    const std::size_t readable = patch::memory::ReadablePrefixSize(
        source, output.size());
    if (readable == 0u) return false;

    std::size_t offset = 0u;
    while (offset < readable) {
        const auto first = std::to_integer<unsigned char>(bytes[offset]);
        output[offset] = bytes[offset];
        if (first == 0u) {
            byteCount = static_cast<std::uint16_t>(offset + 1u);
            return true;
        }
        if ((first & 0x80u) == 0u) {
            ++offset;
            continue;
        }
        if (offset + 1u >= readable) return false;
        output[offset + 1u] = bytes[offset + 1u];
        offset += 2u;
    }
    return false;
}

// Fixed storage keeps captured rich-text bytes at stable addresses while a
// native text rebuild reads them. Synchronization stays with the owner because
// XIII and XIII-2 enter their rebuild paths under different lock scopes
template <typename Record, std::size_t Capacity, std::size_t BucketCount>
class PromptSourceStore {
public:
    struct Lookup {
        std::size_t bucket{BucketCount};
        std::size_t record{Capacity};

        bool found() const { return record < Capacity; }
    };

    static_assert(BucketCount > Capacity);
    static_assert((BucketCount & (BucketCount - 1u)) == 0u);
    static_assert(Capacity < 0xFFFFu);

    Lookup Find(const void* renderText) const {
        if (!renderText) return {};

        std::size_t bucket = Hash(renderText);
        for (std::size_t probe = 0u; probe < BucketCount; ++probe) {
            const std::uint16_t token = index_[bucket];
            if (token == kEmptyBucket) {
                return {bucket, Capacity};
            }
            const std::size_t record = static_cast<std::size_t>(token - 1u);
            if (record < records_.size() &&
                records_[record].renderText == renderText) {
                return {bucket, record};
            }
            bucket = (bucket + 1u) & (BucketCount - 1u);
        }

        return {};
    }

    Record* Insert(void* renderText, const Lookup& lookup) {
        if (!renderText || lookup.bucket >= BucketCount) return nullptr;
        if (lookup.found()) return &records_[lookup.record];

        std::size_t recordIndex = Capacity;
        if (freeCount_ != 0u) {
            recordIndex = free_[--freeCount_];
        } else if (allocated_ < Capacity) {
            recordIndex = allocated_++;
        }
        if (recordIndex >= Capacity) return nullptr;

        Record& record = records_[recordIndex];
        record.renderText = renderText;
        record.byteCount = 0u;
        index_[lookup.bucket] =
            static_cast<std::uint16_t>(recordIndex + 1u);
        return &record;
    }

    void Erase(const Lookup& lookup) {
        if (!lookup.found() || lookup.bucket >= BucketCount) return;

        Record& record = records_[lookup.record];
        record.renderText = nullptr;
        record.byteCount = 0u;
        // Close the probe gap without moving captured text buffers
        std::size_t gap = lookup.bucket;
        for (std::size_t bucket = (gap + 1u) & (BucketCount - 1u);
             index_[bucket] != kEmptyBucket;
             bucket = (bucket + 1u) & (BucketCount - 1u)) {
            const std::size_t home = Hash(records_[index_[bucket] - 1u].renderText);
            if (((gap - home) & (BucketCount - 1u)) <
                ((bucket - home) & (BucketCount - 1u))) {
                index_[gap] = index_[bucket];
                gap = bucket;
            }
        }
        index_[gap] = kEmptyBucket;
        if (freeCount_ < free_.size()) {
            free_[freeCount_++] =
                static_cast<std::uint16_t>(lookup.record);
        }
    }

    Record& At(std::size_t index) { return records_[index]; }
    const Record& At(std::size_t index) const { return records_[index]; }
    std::size_t AllocatedCount() const { return allocated_; }

private:
    static constexpr std::uint16_t kEmptyBucket = 0u;

    static std::size_t Hash(const void* renderText) {
        std::uintptr_t value = reinterpret_cast<std::uintptr_t>(renderText);
        value >>= 4u;
        value ^= value >> 16u;
        value *= static_cast<std::uintptr_t>(0x7FEB352Du);
        value ^= value >> 15u;
        return static_cast<std::size_t>(value) & (BucketCount - 1u);
    }

    std::array<Record, Capacity> records_{};
    std::array<std::uint16_t, BucketCount> index_{};
    std::array<std::uint16_t, Capacity> free_{};
    std::size_t freeCount_{};
    std::size_t allocated_{};
};

} // namespace novafix::game
