#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace novafix::patch::registry {

using ClaimId = std::uint64_t;

enum class Kind : std::uint8_t {
    MemoryWrite,
    Detour,
    ImportHook,
    VtableHook,
};

enum class State : std::uint8_t {
    Reserved,
    Applied,
    Dormant,
    Reverted,
    ContractMismatch,
    WriteFailed,
    OwnershipConflict,
    IntegrityLost,
};

enum class ReserveStatus : std::uint8_t {
    Reserved,
    InvalidDefinition,
    OwnershipConflict,
};

struct SiteDefinition {
    std::byte* address{};
    std::span<const std::byte> expected;
    std::span<const std::byte> replacement;
    Kind kind{Kind::MemoryWrite};
};

struct ReserveResult {
    ReserveStatus status{ReserveStatus::InvalidDefinition};
    ClaimId claim{};
    std::string conflictOwner;
};

struct Summary {
    unsigned reserved{};
    unsigned applied{};
    unsigned dormant{};
    unsigned reverted{};
    unsigned rejected{};
    unsigned conflicts{};
    unsigned integrityLost{};
};

// Reserve all ranges atomically; rejected claims do not block new ones
ReserveResult Reserve(std::string_view feature,
                      std::span<const SiteDefinition> definitions);
void MarkApplied(ClaimId claim);
// Suspend checks while the vtable has no owner, but keep the reservation
void MarkDormant(ClaimId claim);
// Revalidate the claim and restore slots replaced by the runtime
// Preserve valid outer hooks and never adopt foreign pointers
bool ReactivateVtableClaim(ClaimId claim);
void MarkReverted(ClaimId claim);
void MarkFailure(ClaimId claim, State state, std::string_view detail = {});
// Record the bytes installed by an external patch engine
bool CommitExternalPatch(ClaimId claim, std::span<const std::byte> installed);

// Verify installed bytes. Unknown changes fail closed
// siteBudget limits checks per call; the default checks everything
void VerifyIntegrity(std::size_t siteBudget = static_cast<std::size_t>(-1));
Summary CurrentSummary();

} // namespace novafix::patch::registry
