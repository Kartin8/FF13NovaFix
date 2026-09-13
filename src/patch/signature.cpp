#include "patch/signature.h"

#include <cctype>

namespace novafix::patch {
namespace {

int HexDigit(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

} // namespace

std::optional<Signature> Signature::Parse(std::string_view text) {
    Signature result;
    std::size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
        if (position == text.size()) break;

        if (text[position] == '?') {
            ++position;
            if (position < text.size() && text[position] == '?') ++position;
            result.bytes_.push_back(-1);
        } else {
            if (position + 1 >= text.size()) return std::nullopt;
            const int high = HexDigit(text[position]);
            const int low = HexDigit(text[position + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            result.bytes_.push_back(high * 16 + low);
            position += 2;
        }

        if (position < text.size() && !std::isspace(static_cast<unsigned char>(text[position]))) {
            return std::nullopt;
        }
    }
    return result.bytes_.empty() ? std::nullopt : std::optional<Signature>(std::move(result));
}

SignatureMatch Signature::Find(std::span<const std::byte> region) const {
    SignatureMatch result{};
    if (bytes_.empty() || region.size() < bytes_.size()) return result;
    for (std::size_t offset = 0; offset <= region.size() - bytes_.size(); ++offset) {
        bool matches = true;
        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            if (bytes_[index] >= 0 &&
                std::to_integer<unsigned char>(region[offset + index]) != bytes_[index]) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        ++result.count;
        if (result.count == 1) result.address = region.data() + offset;
    }
    if (result.count != 1) result.address = nullptr;
    return result;
}

} // namespace novafix::patch
