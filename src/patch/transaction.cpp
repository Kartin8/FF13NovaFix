#include "patch/transaction.h"

#include "diagnostics/log.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <cstring>

namespace novafix::patch {
namespace {

SRWLOCK g_patchLock = SRWLOCK_INIT;

bool Intersects(std::size_t firstOffset, std::size_t firstSize,
                std::size_t secondOffset, std::size_t secondSize) {
    return firstOffset < secondOffset + secondSize && secondOffset < firstOffset + firstSize;
}

} // namespace

Transaction::Transaction(std::string featureName, std::span<std::byte> region,
                         registry::Kind kind)
    : featureName_(std::move(featureName)), region_(region), kind_(kind) {}

bool Transaction::Add(std::size_t offset, std::span<const std::byte> expected,
                      std::span<const std::byte> replacement) {
    if (expected.empty() || expected.size() != replacement.size()) {
        definitionValid_ = false;
        return false;
    }
    for (const Write& existing : writes_) {
        if (Intersects(offset, expected.size(), existing.offset, existing.expected.size())) {
            overlaps_ = true;
            return false;
        }
    }
    writes_.push_back({offset, {expected.begin(), expected.end()}, {replacement.begin(), replacement.end()}});
    return true;
}

bool Transaction::InRange(const Write& write) const {
    return write.offset <= region_.size() && write.expected.size() <= region_.size() - write.offset;
}

bool Transaction::WriteBytes(const Write& write, bool replacement,
                             bool* bytesWritten) {
    if (bytesWritten) *bytesWritten = false;
    std::byte* destination = region_.data() + write.offset;
    const std::vector<std::byte>& source = replacement ? write.replacement : write.expected;
    const memory::WriteResult result = memory::Write(destination, source);
    if (bytesWritten) *bytesWritten = result.bytesWritten;
    if (!result.protectionsRestored) {
        LogWarning("Memory protection restore failed: feature=%s offset=0x%zX",
            featureName_.c_str(), write.offset);
    }
    if (result.bytesWritten && !result.instructionCacheFlushed) {
        LogWarning("Instruction cache flush failed: feature=%s offset=0x%zX error=%lu",
            featureName_.c_str(), write.offset, GetLastError());
    }
    return result.Succeeded();
}

bool Transaction::RollBackPrefix(std::size_t count) {
    bool restored = true;
    while (count > 0) {
        --count;
        restored = WriteBytes(writes_[count], false) && restored;
    }
    return restored;
}

ApplyStatus Transaction::Apply() {
    AcquireSRWLockExclusive(&g_patchLock);
    if (applied_) {
        ReleaseSRWLockExclusive(&g_patchLock);
        return ApplyStatus::AlreadyApplied;
    }
    if (!definitionValid_ || writes_.empty()) {
        ReleaseSRWLockExclusive(&g_patchLock);
        return ApplyStatus::InvalidDefinition;
    }
    if (overlaps_) {
        ReleaseSRWLockExclusive(&g_patchLock);
        return ApplyStatus::OverlappingWrites;
    }
    for (const Write& write : writes_) {
        if (!InRange(write)) {
            ReleaseSRWLockExclusive(&g_patchLock);
            return ApplyStatus::OutOfRange;
        }
    }
    for (const Write& write : writes_) {
        if (!memory::IsReadable(region_.data() + write.offset,
                                write.expected.size())) {
            ReleaseSRWLockExclusive(&g_patchLock);
            return ApplyStatus::MemoryUnavailable;
        }
    }

    std::vector<registry::SiteDefinition> sites;
    sites.reserve(writes_.size());
    for (const Write& write : writes_) {
        sites.push_back({
            region_.data() + write.offset,
            write.expected,
            write.replacement,
            kind_,
        });
    }
    const registry::ReserveResult reservation = registry::Reserve(featureName_, sites);
    claim_ = reservation.claim;
    if (reservation.status == registry::ReserveStatus::OwnershipConflict) {
        ReleaseSRWLockExclusive(&g_patchLock);
        return ApplyStatus::OwnershipConflict;
    }
    if (reservation.status != registry::ReserveStatus::Reserved) {
        ReleaseSRWLockExclusive(&g_patchLock);
        return ApplyStatus::InvalidDefinition;
    }

    for (const Write& write : writes_) {
        if (std::memcmp(region_.data() + write.offset, write.expected.data(), write.expected.size()) != 0) {
            Log("Patch expected bytes mismatch: feature=%s offset=0x%zX", featureName_.c_str(), write.offset);
            registry::MarkFailure(claim_, registry::State::ContractMismatch,
                                  "expected bytes do not match");
            ReleaseSRWLockExclusive(&g_patchLock);
            return ApplyStatus::ExpectedBytesMismatch;
        }
    }

    for (std::size_t index = 0; index < writes_.size(); ++index) {
        bool bytesWritten = false;
        if (!WriteBytes(writes_[index], true, &bytesWritten)) {
            const bool rolledBack = RollBackPrefix(index + (bytesWritten ? 1u : 0u));
            Log("Patch write failed: feature=%s offset=0x%zX", featureName_.c_str(), writes_[index].offset);
            registry::MarkFailure(claim_, registry::State::WriteFailed,
                                  rolledBack
                                      ? "memory protection, write, or cache flush failed"
                                      : "write failed and rollback was incomplete");
            ReleaseSRWLockExclusive(&g_patchLock);
            return ApplyStatus::ProtectionFailed;
        }
    }
    applied_ = true;
    registry::MarkApplied(claim_);
    ReleaseSRWLockExclusive(&g_patchLock);
    return ApplyStatus::Applied;
}

bool Transaction::Revert() {
    AcquireSRWLockExclusive(&g_patchLock);
    if (!applied_) {
        ReleaseSRWLockExclusive(&g_patchLock);
        return true;
    }
    for (const Write& write : writes_) {
        if (!memory::IsReadable(region_.data() + write.offset,
                                write.replacement.size())) {
            Log("Patch revert failed: memory unavailable feature=%s offset=0x%zX",
                featureName_.c_str(), write.offset);
            registry::MarkFailure(claim_, registry::State::IntegrityLost,
                                  "memory unavailable during revert");
            ReleaseSRWLockExclusive(&g_patchLock);
            return false;
        }
        if (std::memcmp(region_.data() + write.offset, write.replacement.data(), write.replacement.size()) != 0) {
            Log("Patch revert blocked by external modification: feature=%s offset=0x%zX",
                featureName_.c_str(), write.offset);
            registry::MarkFailure(claim_, registry::State::IntegrityLost,
                                  "external modification detected during revert");
            ReleaseSRWLockExclusive(&g_patchLock);
            return false;
        }
    }
    const bool restored = RollBackPrefix(writes_.size());
    if (restored) {
        applied_ = false;
        registry::MarkReverted(claim_);
    } else {
        registry::MarkFailure(claim_, registry::State::WriteFailed,
                              "revert write failed");
    }
    ReleaseSRWLockExclusive(&g_patchLock);
    return restored;
}

bool Transaction::IsApplied() const {
    return applied_;
}

registry::ClaimId Transaction::Claim() const {
    return claim_;
}

const char* ApplyStatusName(ApplyStatus status) {
    switch (status) {
    case ApplyStatus::Applied: return "applied";
    case ApplyStatus::AlreadyApplied: return "already-applied";
    case ApplyStatus::InvalidDefinition: return "invalid-definition";
    case ApplyStatus::OutOfRange: return "out-of-range";
    case ApplyStatus::OverlappingWrites: return "overlapping-writes";
    case ApplyStatus::MemoryUnavailable: return "memory-unavailable";
    case ApplyStatus::ExpectedBytesMismatch: return "expected-bytes-mismatch";
    case ApplyStatus::OwnershipConflict: return "ownership-conflict";
    case ApplyStatus::ProtectionFailed: return "protection-failed";
    }
    return "unknown";
}

} // namespace novafix::patch
