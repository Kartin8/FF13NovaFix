#include "game/titles/ff13_2/rendering/internal_resolution.h"

#include "game/core/image_view.h"
#include "patch/transaction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace novafix::game::xiii2_internal_resolution {
namespace {

struct Branch {
    std::uint32_t address;
    std::array<std::byte, 2> expected;
};

// The port only substitutes 1280x720 with the output size when both dimensions differ
// The substitution already checks for a 1280x720 source, so removing these guards also handles width-only or height-only changes without affecting native 1280x720
constexpr std::array<Branch, 10> kResolutionBranches{{
    {0xC2F9CDu, {std::byte{0x74}, std::byte{0x2A}}},
    {0xC2F9DBu, {std::byte{0x74}, std::byte{0x1C}}},
    {0xC2FF28u, {std::byte{0x74}, std::byte{0x24}}},
    {0xC2FF37u, {std::byte{0x74}, std::byte{0x14}}},
    {0xC328DCu, {std::byte{0x74}, std::byte{0x2A}}},
    {0xC328E8u, {std::byte{0x74}, std::byte{0x1E}}},
    {0xC329BCu, {std::byte{0x74}, std::byte{0x2A}}},
    {0xC329C8u, {std::byte{0x74}, std::byte{0x1E}}},
    {0xC32EACu, {std::byte{0x74}, std::byte{0x2A}}},
    {0xC32EB8u, {std::byte{0x74}, std::byte{0x1E}}},
}};
constexpr std::array<std::byte, 2> kNops{
    std::byte{0x90}, std::byte{0x90}};

// These callers only check whether an override is active
// Change AND to OR, but still report false at native resolution
constexpr std::array<Branch, 2> kOverridePredicates{{
    {0xC2F9AAu, {std::byte{0x74}, std::byte{0x0F}}},
    {0xC2FAECu, {std::byte{0x74}, std::byte{0x34}}},
}};
constexpr std::array<std::byte, 2> kOrBranch{
    std::byte{0x75}, std::byte{0x0C}};

} // namespace

bool RegisterPatches(patch::Transaction& transaction,
                     const ImageView& image) {
    constexpr std::uint32_t kImageBase = 0x400000u;
    for (const Branch& branch : kResolutionBranches) {
        const std::uint32_t rva = branch.address - kImageBase;
        const auto bytes = image.At(rva, branch.expected.size());
        if (!bytes ||
            std::memcmp(bytes->data(), branch.expected.data(),
                        branch.expected.size()) != 0 ||
            !transaction.Add(rva, branch.expected, kNops)) return false;
    }
    for (const Branch& branch : kOverridePredicates) {
        const std::uint32_t rva = branch.address - kImageBase;
        const auto bytes = image.At(rva, branch.expected.size());
        if (!bytes ||
            std::memcmp(bytes->data(), branch.expected.data(),
                        branch.expected.size()) != 0 ||
            !transaction.Add(rva, branch.expected, kOrBranch)) return false;
    }
    return true;
}

} // namespace novafix::game::xiii2_internal_resolution
