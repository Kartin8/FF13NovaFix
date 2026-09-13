#include "game/core/image_view.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace novafix::game {
namespace {

bool IsReadableRange(const void* address, std::size_t size) {
    if (!address || size == 0) return false;
    std::uintptr_t current = reinterpret_cast<std::uintptr_t>(address);
    if (current > (std::numeric_limits<std::uintptr_t>::max)() - size) {
        return false;
    }
    const std::uintptr_t end = current + size;
    while (current < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &information,
                         sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        switch (information.Protect & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            break;
        default:
            return false;
        }
        const auto regionBegin =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        if (regionBegin > (std::numeric_limits<std::uintptr_t>::max)() -
                              information.RegionSize) {
            return false;
        }
        const std::uintptr_t regionEnd = regionBegin + information.RegionSize;
        if (current < regionBegin || current >= regionEnd) return false;
        current = (std::min)(end, regionEnd);
    }
    return true;
}

std::string_view SectionName(const IMAGE_SECTION_HEADER& section) {
    const auto* name = reinterpret_cast<const char*>(section.Name);
    std::size_t length = 0;
    while (length < IMAGE_SIZEOF_SHORT_NAME && name[length] != '\0') ++length;
    return {name, length};
}

} // namespace

ImageView::ImageView(const std::byte* base, const IMAGE_FILE_HEADER* fileHeader,
                     const IMAGE_SECTION_HEADER* sections,
                     std::uint32_t imageSize)
    : base_(base), fileHeader_(fileHeader), sections_(sections),
      imageSize_(imageSize) {}

std::optional<ImageView> ImageView::FromModule(HMODULE module) {
    const auto* base = reinterpret_cast<const std::byte*>(module);
    if (!IsReadableRange(base, sizeof(IMAGE_DOS_HEADER))) {
        return std::nullopt;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        return std::nullopt;
    }

    const auto baseAddress = reinterpret_cast<std::uintptr_t>(base);
    const auto ntOffset = static_cast<std::uintptr_t>(dos->e_lfanew);
    constexpr std::size_t kNtMinimumSize =
        sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(WORD);
    if (baseAddress > (std::numeric_limits<std::uintptr_t>::max)() - ntOffset) {
        return std::nullopt;
    }
    const std::byte* nt = reinterpret_cast<const std::byte*>(baseAddress + ntOffset);
    if (!IsReadableRange(nt, kNtMinimumSize)) return std::nullopt;
    if (*reinterpret_cast<const DWORD*>(nt) != IMAGE_NT_SIGNATURE) return std::nullopt;
    const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(nt + sizeof(DWORD));
    const std::byte* optional = reinterpret_cast<const std::byte*>(fileHeader + 1);
    if (fileHeader->NumberOfSections == 0 || fileHeader->SizeOfOptionalHeader < sizeof(WORD)) {
        return std::nullopt;
    }
    if (!IsReadableRange(optional, fileHeader->SizeOfOptionalHeader)) {
        return std::nullopt;
    }

    const WORD magic = *reinterpret_cast<const WORD*>(optional);
    std::uint32_t imageSize{};
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
        imageSize = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(optional)->SizeOfImage;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
               fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
        imageSize = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(optional)->SizeOfImage;
    } else {
        return std::nullopt;
    }
    if (!imageSize) return std::nullopt;

    if (baseAddress > (std::numeric_limits<std::uintptr_t>::max)() - imageSize) {
        return std::nullopt;
    }
    const auto optionalAddress = reinterpret_cast<std::uintptr_t>(optional);
    if (optionalAddress > (std::numeric_limits<std::uintptr_t>::max)() -
                              fileHeader->SizeOfOptionalHeader) {
        return std::nullopt;
    }
    const auto sectionsAddress = optionalAddress + fileHeader->SizeOfOptionalHeader;
    constexpr std::size_t kSectionSize = sizeof(IMAGE_SECTION_HEADER);
    const std::size_t sectionTableSize =
        static_cast<std::size_t>(fileHeader->NumberOfSections) * kSectionSize;
    const auto imageEnd = baseAddress + imageSize;
    if (sectionsAddress > imageEnd || sectionTableSize > imageEnd - sectionsAddress ||
        !IsReadableRange(
            reinterpret_cast<const void*>(sectionsAddress), sectionTableSize)) {
        return std::nullopt;
    }
    const auto* sections =
        reinterpret_cast<const IMAGE_SECTION_HEADER*>(sectionsAddress);
    return ImageView(base, fileHeader, sections, imageSize);
}

std::span<const std::byte> ImageView::Bytes() const {
    return {base_, imageSize_};
}

std::optional<std::span<const std::byte>> ImageView::At(
    std::uint32_t rva, std::size_t size) const {
    if (size == 0 || rva > imageSize_ || size > imageSize_ - rva) {
        return std::nullopt;
    }
    const auto* address = base_ + rva;
    if (!IsReadableRange(address, size)) return std::nullopt;
    return std::span<const std::byte>(address, size);
}

std::optional<std::uint32_t> ImageView::Address32(
    std::uint32_t rva, std::size_t size) const {
    if (!At(rva, size)) return std::nullopt;
    const auto base = reinterpret_cast<std::uintptr_t>(base_);
    if (base > (std::numeric_limits<std::uint32_t>::max)() - rva) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(base + rva);
}

std::optional<SectionView> ImageView::Section(std::string_view name) const {
    const std::size_t sectionTableSize =
        static_cast<std::size_t>(fileHeader_->NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (!IsReadableRange(sections_, sectionTableSize)) {
        return std::nullopt;
    }
    for (unsigned index = 0; index < fileHeader_->NumberOfSections; ++index) {
        const IMAGE_SECTION_HEADER& section = sections_[index];
        if (SectionName(section) != name) continue;
        const std::uint32_t rva = section.VirtualAddress;
        const std::uint32_t requestedSize = section.Misc.VirtualSize != 0
            ? section.Misc.VirtualSize
            : section.SizeOfRawData;
        if (rva >= imageSize_) return std::nullopt;
        const std::uint32_t available = imageSize_ - rva;
        const std::uint32_t size = std::min(requestedSize, available);
        const auto bytes = At(rva, size);
        if (!bytes) return std::nullopt;
        return SectionView{*bytes};
    }
    return std::nullopt;
}

bool ImageView::Contains(const void* address, std::size_t size) const {
    if (!address || size == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(base_);
    if (begin > (std::numeric_limits<std::uintptr_t>::max)() - imageSize_) {
        return false;
    }
    const auto end = begin + imageSize_;
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= begin && value <= end && size <= end - value &&
           IsReadableRange(address, size);
}

std::optional<std::uint32_t> ImageView::Rva(const void* address) const {
    if (!Contains(address, 1)) return std::nullopt;
    return static_cast<std::uint32_t>(reinterpret_cast<const std::byte*>(address) - base_);
}

} // namespace novafix::game
