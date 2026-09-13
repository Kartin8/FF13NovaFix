#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace novafix::game {

struct SectionView {
    std::span<const std::byte> bytes;
};

struct ImageFingerprint {
    std::uint16_t machine{};
    std::uint16_t optionalHeaderMagic{};
    std::uint32_t timeDateStamp{};
    std::uint32_t imageSize{};
    std::uint32_t textSize{};
    std::uint64_t textHash{};
};

class ImageView {
public:
    static std::optional<ImageView> FromModule(HMODULE module);

    std::span<const std::byte> Bytes() const;
    std::optional<std::span<const std::byte>> At(
        std::uint32_t rva, std::size_t size) const;
    std::optional<std::uint32_t> Address32(
        std::uint32_t rva, std::size_t size = 1u) const;
    std::optional<SectionView> Section(std::string_view name) const;
    bool Contains(const void* address, std::size_t size) const;
    std::optional<std::uint32_t> Rva(const void* address) const;

private:
    ImageView(const std::byte* base, const IMAGE_FILE_HEADER* fileHeader,
              const IMAGE_SECTION_HEADER* sections, std::uint32_t imageSize);

    const std::byte* base_{};
    const IMAGE_FILE_HEADER* fileHeader_{};
    const IMAGE_SECTION_HEADER* sections_{};
    std::uint32_t imageSize_{};
};

} // namespace novafix::game
