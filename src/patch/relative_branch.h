#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace novafix::game {
class ImageView;
}

namespace novafix::patch {

class Transaction;

struct RelativeCallSite {
    std::uint32_t rva{};
    std::array<std::uint8_t, 5> expected{};
};

// Encode a near CALL without touching memory; callers can validate the patch site first
std::optional<std::array<std::byte, 5>> EncodeRelativeCall32(
    std::uintptr_t instruction, std::uintptr_t destination);

bool AddRelativeCalls(Transaction& transaction, const game::ImageView& image,
                      std::span<const RelativeCallSite> sites,
                      const void* destination);

} // namespace novafix::patch
