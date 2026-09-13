#include "patch/relative_branch.h"

#include "game/core/image_view.h"
#include "patch/transaction.h"

#include <cstring>
#include <limits>
#include <span>

namespace novafix::patch {

std::optional<std::array<std::byte, 5>> EncodeRelativeCall32(
    std::uintptr_t instruction, std::uintptr_t destination) {
    if (instruction > (std::numeric_limits<std::uintptr_t>::max)() - 5u) {
        return std::nullopt;
    }
    const std::int64_t displacement =
        static_cast<std::int64_t>(destination) -
        static_cast<std::int64_t>(instruction + 5u);
    if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
        displacement > (std::numeric_limits<std::int32_t>::max)()) {
        return std::nullopt;
    }

    std::array<std::byte, 5> encoded{};
    encoded[0] = std::byte{0xE8};
    const std::int32_t relative = static_cast<std::int32_t>(displacement);
    std::memcpy(encoded.data() + 1u, &relative, sizeof(relative));
    return encoded;
}

bool AddRelativeCalls(Transaction& transaction, const game::ImageView& image,
                      std::span<const RelativeCallSite> sites,
                      const void* destination) {
    for (const RelativeCallSite& site : sites) {
        const auto address = image.At(site.rva, site.expected.size());
        if (!address) return false;
        const auto replacement = EncodeRelativeCall32(
            reinterpret_cast<std::uintptr_t>(address->data()),
            reinterpret_cast<std::uintptr_t>(destination));
        if (!replacement ||
            !transaction.Add(
                site.rva, std::as_bytes(std::span{site.expected}),
                *replacement)) {
            return false;
        }
    }
    return true;
}

} // namespace novafix::patch
