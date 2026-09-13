#pragma once

#include "patch/registry.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace novafix::patch {

// MinHook handles the trampoline; expectedPrefix verifies the target bytes
class Detour32 {
public:
    Detour32() = default;
    Detour32(const Detour32&) = delete;
    Detour32& operator=(const Detour32&) = delete;
    ~Detour32() = default;

    bool Install(std::string_view featureName, void* target, void* hook,
                 std::span<const std::byte> expectedPrefix,
                 void** originalOut = nullptr);
    bool Revert();
    bool IsInstalled() const;

    template <typename Original>
    bool RevertAndClear(Original& original) {
        static_assert(std::is_pointer_v<Original>);
        if (!IsInstalled() || Revert()) {
            original = nullptr;
            return true;
        }
        return false;
    }

private:
    enum class State : unsigned char {
        Inactive,
        Enabled,
        Disabled,
    };

    void* target_{};
    void* trampoline_{};
    registry::ClaimId claim_{};
    std::vector<std::byte> expectedPrefix_;
    std::vector<std::byte> installedPrefix_;
    State state_{State::Inactive};
};

} // namespace novafix::patch
