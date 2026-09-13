#pragma once

#include "patch/registry.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace novafix::patch {

enum class ApplyStatus {
    Applied,
    AlreadyApplied,
    InvalidDefinition,
    OutOfRange,
    OverlappingWrites,
    MemoryUnavailable,
    ExpectedBytesMismatch,
    OwnershipConflict,
    ProtectionFailed,
};

class Transaction {
public:
    Transaction(std::string featureName, std::span<std::byte> region,
                registry::Kind kind = registry::Kind::MemoryWrite);

    bool Add(std::size_t offset, std::span<const std::byte> expected,
             std::span<const std::byte> replacement);
    ApplyStatus Apply();
    bool Revert();
    bool IsApplied() const;
    registry::ClaimId Claim() const;

private:
    struct Write {
        std::size_t offset{};
        std::vector<std::byte> expected;
        std::vector<std::byte> replacement;
    };

    bool InRange(const Write& write) const;
    bool WriteBytes(const Write& write, bool replacement,
                    bool* bytesWritten = nullptr);
    bool RollBackPrefix(std::size_t count);

    std::string featureName_;
    std::span<std::byte> region_;
    std::vector<Write> writes_;
    registry::Kind kind_{registry::Kind::MemoryWrite};
    registry::ClaimId claim_{};
    bool definitionValid_{true};
    bool overlaps_{false};
    bool applied_{false};
};

const char* ApplyStatusName(ApplyStatus status);

} // namespace novafix::patch
