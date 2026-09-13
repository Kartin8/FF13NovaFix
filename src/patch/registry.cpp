#include "patch/registry.h"

#include "diagnostics/log.h"
#include "common/module_path.h"
#include "patch/memory_access.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace novafix::patch::registry {
namespace {

struct Site {
    std::byte* address{};
    std::vector<std::byte> expected;
    std::vector<std::byte> replacement;
    Kind kind{Kind::MemoryWrite};
};

struct Claim {
    ClaimId id{};
    std::string feature;
    State state{State::Reserved};
    std::string detail;
    std::vector<Site> sites;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Claim> g_claims;
ClaimId g_nextClaim{1};
std::size_t g_verificationClaim{};
std::size_t g_verificationSite{};

bool OwnsMemory(State state) {
    return state == State::Reserved || state == State::Applied ||
           state == State::Dormant;
}

bool IsIssue(State state) {
    return state == State::ContractMismatch || state == State::WriteFailed ||
           state == State::OwnershipConflict || state == State::IntegrityLost;
}

bool ValidRange(const std::byte* address, std::size_t size) {
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    return address && size != 0 &&
           begin <= std::numeric_limits<std::uintptr_t>::max() - size;
}

bool Intersects(const std::byte* firstAddress, std::size_t firstSize,
                const std::byte* secondAddress, std::size_t secondSize) {
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(firstAddress);
    const std::uintptr_t second = reinterpret_cast<std::uintptr_t>(secondAddress);
    return first < second + secondSize && second < first + firstSize;
}

Claim* Find(ClaimId id) {
    for (Claim& claim : g_claims) {
        if (claim.id == id) return &claim;
    }
    return nullptr;
}

std::vector<Site> CopySites(std::span<const SiteDefinition> definitions) {
    std::vector<Site> sites;
    sites.reserve(definitions.size());
    for (const SiteDefinition& definition : definitions) {
        sites.push_back({
            definition.address,
            {definition.expected.begin(), definition.expected.end()},
            {definition.replacement.begin(), definition.replacement.end()},
            definition.kind,
        });
    }
    return sites;
}

bool Readable(const Site& site) {
    return memory::IsReadable(site.address, site.replacement.size());
}

bool ExecutableAddress(const void* address) {
    return memory::IsExecutable(address, 1u);
}

bool IsPointerHook(const Site& site) {
    return site.kind == Kind::VtableHook || site.kind == Kind::ImportHook;
}

bool ModuleForAddress(const void* address, HMODULE& module) {
    module = nullptr;
    return address && GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &module) && module;
}

bool IsExternalPointerLayer(const Site& site,
                            std::wstring* ownerPath = nullptr,
                            bool* systemCompatibilityLayer = nullptr) {
    if (!IsPointerHook(site) ||
        site.replacement.size() != sizeof(void*) || !Readable(site)) {
        return false;
    }

    void* target = nullptr;
    void* expected = nullptr;
    void* replacement = nullptr;
    std::memcpy(&target, site.address, sizeof(target));
    std::memcpy(&expected, site.expected.data(), sizeof(expected));
    std::memcpy(&replacement, site.replacement.data(), sizeof(replacement));
    if (target == expected || target == replacement) return false;
    if (!ExecutableAddress(target)) return false;

    HMODULE owner = nullptr;
    HMODULE replacementOwner = nullptr;
    if (!ModuleForAddress(target, owner) ||
        !ModuleForAddress(replacement, replacementOwner) ||
        owner == replacementOwner) return false;

    std::wstring loadedPath = path::ModuleFile(owner);
    if (loadedPath.empty()) return false;
    const std::wstring systemApphelp = path::SystemFile(L"apphelp.dll");
    if (systemCompatibilityLayer) {
        *systemCompatibilityLayer = !systemApphelp.empty() &&
            path::Equivalent(loadedPath, systemApphelp);
    }
    if (ownerPath) *ownerPath = std::move(loadedPath);
    return true;
}

memory::WriteResult WriteSiteBytes(const Site& site,
                                   std::span<const std::byte> bytes) {
    if (bytes.size() != site.replacement.size()) return {};
    const memory::WriteResult result = memory::Write(site.address, bytes);
    if (!result.protectionsRestored) {
        LogWarning("Could not restore memory protection: address=%p",
            site.address);
    }
    return result;
}

} // namespace

ReserveResult Reserve(std::string_view feature,
                      std::span<const SiteDefinition> definitions) {
    if (feature.empty() || definitions.empty()) return {};
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const SiteDefinition& definition = definitions[index];
        if (!ValidRange(definition.address, definition.expected.size()) ||
            definition.expected.size() != definition.replacement.size() ||
            !memory::IsReadable(definition.address,
                                definition.expected.size())) {
            return {};
        }
        for (std::size_t other = index + 1; other < definitions.size(); ++other) {
            if (Intersects(definition.address, definition.expected.size(),
                           definitions[other].address, definitions[other].expected.size())) {
                return {};
            }
        }
    }

    AcquireSRWLockExclusive(&g_lock);
    Claim claim{};
    claim.id = g_nextClaim++;
    claim.feature = feature;
    claim.sites = CopySites(definitions);

    for (const Claim& existing : g_claims) {
        if (!OwnsMemory(existing.state)) continue;
        for (const Site& requested : claim.sites) {
            for (const Site& owned : existing.sites) {
                if (!Intersects(requested.address, requested.expected.size(),
                                owned.address, owned.expected.size())) {
                    continue;
                }
                claim.state = State::OwnershipConflict;
                claim.detail = existing.feature;
                const ClaimId id = claim.id;
                const std::string conflictOwner = claim.detail;
                g_claims.push_back(std::move(claim));
                ReleaseSRWLockExclusive(&g_lock);
                Log("Patch ownership conflict: feature=%.*s owner=%s address=%p size=%zu",
                    static_cast<int>(feature.size()), feature.data(), conflictOwner.c_str(),
                    requested.address, requested.expected.size());
                return {ReserveStatus::OwnershipConflict, id, conflictOwner};
            }
        }
    }

    const ClaimId id = claim.id;
    g_claims.push_back(std::move(claim));
    ReleaseSRWLockExclusive(&g_lock);
    return {ReserveStatus::Reserved, id, {}};
}

void MarkApplied(ClaimId claimId) {
    AcquireSRWLockExclusive(&g_lock);
    Claim* claim = Find(claimId);
    if (claim && (claim->state == State::Reserved || claim->state == State::Dormant)) {
        claim->state = State::Applied;
        claim->detail.clear();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void MarkDormant(ClaimId claimId) {
    if (!claimId) return;
    AcquireSRWLockExclusive(&g_lock);
    Claim* claim = Find(claimId);
    if (claim && claim->state == State::Applied &&
        std::all_of(claim->sites.begin(), claim->sites.end(),
                    [](const Site& site) { return site.kind == Kind::VtableHook; })) {
        claim->state = State::Dormant;
        claim->detail = "owning COM object was released";
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool ReactivateVtableClaim(ClaimId claimId) {
    if (!claimId) return false;
    AcquireSRWLockExclusive(&g_lock);
    Claim* claim = Find(claimId);
    if (!claim ||
        (claim->state != State::Dormant && claim->state != State::Applied) ||
        claim->sites.empty() ||
        !std::all_of(claim->sites.begin(), claim->sites.end(),
                     [](const Site& site) {
                         return site.kind == Kind::VtableHook && Readable(site);
                     })) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    std::vector<std::size_t> restoredSites;
    restoredSites.reserve(claim->sites.size());
    std::wstring externalOwner;
    bool hasSystemCompatibilityLayer = false;
    for (std::size_t index = 0; index < claim->sites.size(); ++index) {
        const Site& site = claim->sites[index];
        if (std::memcmp(site.address, site.replacement.data(),
                        site.replacement.size()) == 0) {
            continue;
        }
        if (std::memcmp(site.address, site.expected.data(),
                        site.expected.size()) == 0) {
            restoredSites.push_back(index);
            continue;
        }
        std::wstring ownerPath;
        bool systemCompatibilityLayer = false;
        if (IsExternalPointerLayer(
                site, &ownerPath, &systemCompatibilityLayer)) {
            externalOwner = std::move(ownerPath);
            hasSystemCompatibilityLayer |= systemCompatibilityLayer;
            continue;
        }
        claim->state = State::IntegrityLost;
        claim->detail = "vtable slot changed by an unknown owner";
        Log("Vtable reactivation refused: feature=%s address=%p",
            claim->feature.c_str(), site.address);
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    std::size_t appliedCount = 0;
    for (const std::size_t index : restoredSites) {
        const memory::WriteResult write = WriteSiteBytes(
            claim->sites[index], claim->sites[index].replacement);
        if (write.Succeeded()) {
            ++appliedCount;
            continue;
        }
        if (write.bytesWritten) {
            WriteSiteBytes(claim->sites[index],
                           claim->sites[index].expected);
        }
        while (appliedCount > 0) {
            --appliedCount;
            const std::size_t rollbackIndex = restoredSites[appliedCount];
            WriteSiteBytes(claim->sites[rollbackIndex],
                           claim->sites[rollbackIndex].expected);
        }
        claim->state = State::WriteFailed;
        claim->detail = "vtable reactivation write failed";
        Log("Vtable reactivation failed: feature=%s address=%p",
            claim->feature.c_str(), claim->sites[index].address);
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    claim->state = State::Applied;
    if (hasSystemCompatibilityLayer) {
        claim->detail = "vtable hook chained through Windows apphelp";
    } else if (!externalOwner.empty()) {
        claim->detail = "vtable hook layered by external module";
    } else {
        claim->detail.clear();
    }
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

void MarkReverted(ClaimId claimId) {
    AcquireSRWLockExclusive(&g_lock);
    Claim* claim = Find(claimId);
    if (claim) {
        claim->state = State::Reverted;
        claim->detail.clear();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void MarkFailure(ClaimId claimId, State state, std::string_view detail) {
    if (!IsIssue(state)) return;
    AcquireSRWLockExclusive(&g_lock);
    Claim* claim = Find(claimId);
    if (claim) {
        claim->state = state;
        claim->detail = detail;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool CommitExternalPatch(ClaimId claimId,
                         std::span<const std::byte> installed) {
    if (!claimId || installed.empty()) return false;
    AcquireSRWLockExclusive(&g_lock);
    Claim* claim = Find(claimId);
    if (!claim || claim->state != State::Reserved ||
        claim->sites.size() != 1 ||
        claim->sites[0].replacement.size() != installed.size() ||
        !Readable(claim->sites[0]) ||
        std::memcmp(claim->sites[0].address, installed.data(),
                    installed.size()) != 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    claim->sites[0].replacement.assign(installed.begin(), installed.end());
    claim->state = State::Applied;
    claim->detail.clear();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

void VerifyIntegrity(std::size_t siteBudget) {
    if (siteBudget == 0) return;
    AcquireSRWLockExclusive(&g_lock);
    std::size_t totalSites = 0;
    for (const Claim& claim : g_claims) totalSites += claim.sites.size();
    if (totalSites == 0) {
        g_verificationClaim = 0;
        g_verificationSite = 0;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    std::size_t visited = 0;
    while (visited < totalSites && visited < siteBudget) {
        if (g_verificationClaim >= g_claims.size()) {
            g_verificationClaim = 0;
            g_verificationSite = 0;
        }
        Claim& claim = g_claims[g_verificationClaim];
        if (g_verificationSite >= claim.sites.size()) {
            ++g_verificationClaim;
            g_verificationSite = 0;
            continue;
        }

        const Site& site = claim.sites[g_verificationSite++];
        ++visited;
        if (claim.state != State::Applied) continue;
        if (Readable(site) &&
            std::memcmp(site.address, site.replacement.data(),
                        site.replacement.size()) == 0) {
            continue;
        }
        std::wstring externalOwner;
        bool systemCompatibilityLayer = false;
        if (IsExternalPointerLayer(
                site, &externalOwner, &systemCompatibilityLayer)) {
            const std::string detail = systemCompatibilityLayer
                ? "vtable hook chained through Windows apphelp"
                : "pointer hook layered by external module";
            if (claim.detail != detail) {
                claim.detail = detail;
                Log("Another DLL chained the pointer hook: feature=%s address=%p owner=%ls",
                    claim.feature.c_str(), site.address,
                    externalOwner.c_str());
            }
            continue;
        }
        claim.state = State::IntegrityLost;
        claim.detail = "installed bytes were modified";
        if (site.replacement.size() == sizeof(void*)) {
            void* actual = nullptr;
            void* expected = nullptr;
            void* replacement = nullptr;
            if (Readable(site)) {
                std::memcpy(&actual, site.address, sizeof(actual));
            }
            std::memcpy(&expected, site.expected.data(), sizeof(expected));
            std::memcpy(
                &replacement, site.replacement.data(), sizeof(replacement));
            HMODULE actualModule = nullptr;
            wchar_t actualModulePath[32768]{};
            if (actual) {
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(actual), &actualModule);
            }
            if (actualModule) {
                GetModuleFileNameW(
                    actualModule, actualModulePath,
                    static_cast<DWORD>(std::size(actualModulePath)));
            }
            LogError("Patch integrity lost: feature=%s address=%p size=%zu "
                "actual=%p expected=%p replacement=%p owner=%ls",
                claim.feature.c_str(), site.address, site.replacement.size(),
                actual, expected, replacement,
                actualModulePath[0] ? actualModulePath : L"<unknown>");
        } else {
            LogError("Patch integrity lost: feature=%s address=%p size=%zu",
                claim.feature.c_str(), site.address, site.replacement.size());
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

Summary CurrentSummary() {
    Summary summary{};
    AcquireSRWLockShared(&g_lock);
    for (const Claim& claim : g_claims) {
        switch (claim.state) {
        case State::Reserved: ++summary.reserved; break;
        case State::Applied: ++summary.applied; break;
        case State::Dormant: ++summary.dormant; break;
        case State::Reverted: ++summary.reverted; break;
        case State::ContractMismatch:
        case State::WriteFailed: ++summary.rejected; break;
        case State::OwnershipConflict: ++summary.conflicts; break;
        case State::IntegrityLost: ++summary.integrityLost; break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return summary;
}

} // namespace novafix::patch::registry
