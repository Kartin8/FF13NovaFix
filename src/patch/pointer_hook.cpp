#include "patch/pointer_hook.h"

#include "patch/memory_access.h"
#include "patch/transaction.h"

#include <array>
#include <cstring>
#include <string>

namespace novafix::patch {

bool InstallPointerHook(std::string_view feature, void** slot, void* replacement,
                        void** original, registry::Kind kind,
                        registry::ClaimId* claim) {
    if (feature.empty() || !slot || !replacement || !original ||
        (kind != registry::Kind::ImportHook &&
         kind != registry::Kind::VtableHook)) {
        return false;
    }
    *original = nullptr;
    if (!memory::IsReadable(slot, sizeof(*slot))) return false;
    // If this slot already points to us, let the registry preserve the original target
    if (*slot == replacement) return false;

    void* current = *slot;
    std::array<std::byte, sizeof(void*)> expected{};
    std::array<std::byte, sizeof(void*)> patched{};
    std::memcpy(expected.data(), &current, sizeof(current));
    std::memcpy(patched.data(), &replacement, sizeof(replacement));

    Transaction transaction(
        std::string(feature), {reinterpret_cast<std::byte*>(slot), sizeof(void*)}, kind);
    if (!transaction.Add(0, expected, patched)) return false;
    *original = current;
    if (transaction.Apply() != ApplyStatus::Applied) {
        *original = nullptr;
        return false;
    }
    if (claim) *claim = transaction.Claim();
    return true;
}

} // namespace novafix::patch
