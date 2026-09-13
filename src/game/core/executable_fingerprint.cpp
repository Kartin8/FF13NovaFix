#include "game/core/executable_fingerprint.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace novafix::game {
namespace {

constexpr std::uint64_t kMaximumExecutableSize = 512ull * 1024ull * 1024ull;

bool RangeInFile(std::size_t offset, std::size_t size, std::size_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

std::string_view SectionName(const IMAGE_SECTION_HEADER& section) {
    const auto* name = reinterpret_cast<const char*>(section.Name);
    std::size_t length = 0;
    while (length < IMAGE_SIZEOF_SHORT_NAME && name[length] != '\0') ++length;
    return {name, length};
}

void HashByte(std::uint64_t* hash, std::uint8_t value) {
    *hash ^= value;
    *hash *= 1099511628211ull;
}

} // namespace

std::optional<ImageFingerprint> FingerprintExecutableFile(
    std::wstring_view filePath) {
    if (filePath.empty()) return std::nullopt;
    const std::wstring path(filePath);
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaximumExecutableSize) {
        CloseHandle(file);
        return std::nullopt;
    }
    const std::size_t fileSize = static_cast<std::size_t>(size.QuadPart);
    HANDLE mapping = CreateFileMappingW(
        file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const auto* base = mapping
        ? static_cast<const std::byte*>(
              MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0))
        : nullptr;
    if (!base) {
        if (mapping) CloseHandle(mapping);
        CloseHandle(file);
        return std::nullopt;
    }

    std::optional<ImageFingerprint> result;
    if (RangeInFile(0, sizeof(IMAGE_DOS_HEADER), fileSize)) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const std::size_t ntOffset = dos->e_lfanew > 0
            ? static_cast<std::size_t>(dos->e_lfanew) : fileSize;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
            RangeInFile(ntOffset, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER),
                        fileSize) &&
            *reinterpret_cast<const DWORD*>(base + ntOffset) ==
                IMAGE_NT_SIGNATURE) {
            const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(
                base + ntOffset + sizeof(DWORD));
            const std::size_t optionalOffset =
                ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
            const std::size_t sectionOffset =
                optionalOffset + fileHeader->SizeOfOptionalHeader;
            if (fileHeader->NumberOfSections != 0 &&
                fileHeader->SizeOfOptionalHeader >= sizeof(WORD) &&
                RangeInFile(optionalOffset, fileHeader->SizeOfOptionalHeader,
                            fileSize) &&
                RangeInFile(sectionOffset,
                            static_cast<std::size_t>(fileHeader->NumberOfSections) *
                                sizeof(IMAGE_SECTION_HEADER),
                            fileSize)) {
                const auto* optional = base + optionalOffset;
                const WORD magic = *reinterpret_cast<const WORD*>(optional);
                std::uint32_t imageSize = 0;
                if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
                    fileHeader->SizeOfOptionalHeader >=
                        sizeof(IMAGE_OPTIONAL_HEADER32)) {
                    imageSize =
                        reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(optional)
                            ->SizeOfImage;
                } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                           fileHeader->SizeOfOptionalHeader >=
                               sizeof(IMAGE_OPTIONAL_HEADER64)) {
                    imageSize =
                        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(optional)
                            ->SizeOfImage;
                }

                const auto* sections =
                    reinterpret_cast<const IMAGE_SECTION_HEADER*>(
                        base + sectionOffset);
                for (unsigned index = 0;
                     imageSize && index < fileHeader->NumberOfSections; ++index) {
                    const IMAGE_SECTION_HEADER& section = sections[index];
                    if (SectionName(section) != ".text") continue;
                    const std::uint32_t virtualSize =
                        section.Misc.VirtualSize != 0
                            ? section.Misc.VirtualSize
                            : section.SizeOfRawData;
                    const std::size_t hashedRawSize =
                        std::min<std::size_t>(
                            virtualSize, section.SizeOfRawData);
                    if (!virtualSize ||
                        !RangeInFile(section.PointerToRawData, hashedRawSize,
                                     fileSize)) {
                        break;
                    }

                    std::uint64_t hash = 14695981039346656037ull;
                    const auto* raw = base + section.PointerToRawData;
                    for (std::size_t byte = 0; byte < hashedRawSize; ++byte) {
                        HashByte(&hash, std::to_integer<std::uint8_t>(raw[byte]));
                    }
                    for (std::size_t byte = hashedRawSize;
                         byte < virtualSize; ++byte) {
                        HashByte(&hash, 0);
                    }
                    result = ImageFingerprint{
                        fileHeader->Machine,
                        magic,
                        fileHeader->TimeDateStamp,
                        imageSize,
                        virtualSize,
                        hash,
                    };
                    break;
                }
            }
        }
    }

    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return result;
}

} // namespace novafix::game
