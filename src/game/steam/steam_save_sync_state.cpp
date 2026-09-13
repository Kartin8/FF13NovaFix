#include "game/steam/steam_save_sync_state.h"

#include "game/steam/steam_save_files.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace novafix::game::steam_cloud::sync_state {
namespace {

constexpr wchar_t kFileName[] = L"NovaFix.cloud-sync-state";
constexpr std::string_view kHeader = "novafix-cloud-sync-v1\n";
constexpr std::int32_t kMaximumStateBytes = 64 * 1024;
constexpr std::size_t kMaximumEntries = 256;

char HexDigit(unsigned value) {
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('a' + value - 10);
}

bool HexValue(char value, unsigned char& result) {
    if (value >= '0' && value <= '9') {
        result = static_cast<unsigned char>(value - '0');
        return true;
    }
    if (value >= 'a' && value <= 'f') {
        result = static_cast<unsigned char>(value - 'a' + 10);
        return true;
    }
    if (value >= 'A' && value <= 'F') {
        result = static_cast<unsigned char>(value - 'A' + 10);
        return true;
    }
    return false;
}

std::string EncodeDigest(
    const std::array<unsigned char, 32>& digest) {
    std::string encoded;
    encoded.reserve(digest.size() * 2);
    for (const unsigned char value : digest) {
        encoded.push_back(HexDigit(value >> 4));
        encoded.push_back(HexDigit(value & 0x0f));
    }
    return encoded;
}

bool DecodeDigest(
    std::string_view encoded,
    std::array<unsigned char, 32>& digest) {
    if (encoded.size() != digest.size() * 2) return false;
    for (std::size_t index = 0; index < digest.size(); ++index) {
        unsigned char high{};
        unsigned char low{};
        if (!HexValue(encoded[index * 2], high) ||
            !HexValue(encoded[index * 2 + 1], low)) {
            return false;
        }
        digest[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

bool ParseLine(std::string_view line, Entry& entry) {
    const std::size_t first = line.find('\t');
    const std::size_t second = first == std::string_view::npos
        ? std::string_view::npos : line.find('\t', first + 1);
    if (first == std::string_view::npos ||
        second == std::string_view::npos ||
        line.find('\t', second + 1) != std::string_view::npos) {
        return false;
    }

    entry = {};
    entry.name.assign(line.substr(0, first));
    if (!save_files::IsSafeSaveName(entry.name.c_str())) return false;

    const std::string_view sizeText = line.substr(
        first + 1, second - first - 1);
    std::uint64_t size{};
    const auto parsed = std::from_chars(
        sizeText.data(), sizeText.data() + sizeText.size(), size);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != sizeText.data() + sizeText.size() ||
        size > static_cast<std::uint64_t>(
            save_files::kMaximumSaveBytes)) {
        return false;
    }
    entry.identity.size = size;
    return DecodeDigest(line.substr(second + 1), entry.identity.sha256);
}

} // namespace

bool Load(std::vector<Entry>& entries) {
    entries.clear();
    const std::wstring root = save_files::Root();
    if (root.empty()) return false;

    std::vector<char> bytes;
    if (!save_files::ReadWholeFile(
            root + L'\\' + kFileName, bytes, kMaximumStateBytes)) {
        return true;
    }
    if (bytes.empty()) return false;
    const std::string_view contents(bytes.data(), bytes.size());
    if (!contents.starts_with(kHeader)) return false;

    std::size_t offset = kHeader.size();
    while (offset < contents.size()) {
        const std::size_t end = contents.find('\n', offset);
        const std::string_view line = contents.substr(
            offset, end == std::string_view::npos
                ? contents.size() - offset : end - offset);
        if (line.empty()) return false;
        Entry entry;
        if (!ParseLine(line, entry) || entries.size() >= kMaximumEntries) {
            entries.clear();
            return false;
        }
        const auto duplicate = std::find_if(
            entries.begin(), entries.end(), [&](const Entry& existing) {
                return _stricmp(existing.name.c_str(), entry.name.c_str()) == 0;
            });
        if (duplicate != entries.end()) {
            entries.clear();
            return false;
        }
        entries.push_back(std::move(entry));
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return true;
}

bool Store(const std::vector<Entry>& entries) {
    if (entries.size() > kMaximumEntries) return false;
    const std::wstring root = save_files::Root();
    if (!save_files::EnsureDirectory(root)) return false;

    std::vector<Entry> ordered = entries;
    std::sort(ordered.begin(), ordered.end(),
              [](const Entry& left, const Entry& right) {
                  return _stricmp(left.name.c_str(), right.name.c_str()) < 0;
              });

    std::string contents(kHeader);
    for (const Entry& entry : ordered) {
        if (!save_files::IsSafeSaveName(entry.name.c_str()) ||
            entry.identity.size > static_cast<std::uint64_t>(
                save_files::kMaximumSaveBytes)) {
            return false;
        }
        contents.append(entry.name);
        contents.push_back('\t');
        contents.append(std::to_string(entry.identity.size));
        contents.push_back('\t');
        contents.append(EncodeDigest(entry.identity.sha256));
        contents.push_back('\n');
    }
    if (contents.size() > static_cast<std::size_t>(kMaximumStateBytes)) {
        return false;
    }
    return save_files::AtomicWritePath(
        root + L'\\' + kFileName, contents.data(),
        static_cast<std::int32_t>(contents.size()), false);
}

} // namespace novafix::game::steam_cloud::sync_state
