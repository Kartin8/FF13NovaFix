#include "compat/addons/native_notification_text.h"

#include <array>
#include <utility>

namespace novafix::compat::native_notification_text {
namespace {

struct Token {
    std::wstring_view text;
    wchar_t glyph;
};

constexpr std::array<Token, 5> kTokens{{
    {L"{button:confirm}", kConfirmGlyph},
    {L"{button:cancel}", kCancelGlyph},
    {L"{button:face-left}", kFaceLeftGlyph},
    {L"{button:face-top}", kFaceTopGlyph},
    {L"{button:menu}", kMenuGlyph},
}};

} // namespace

bool ExpandMarkup(std::wstring& text) {
    std::wstring expanded;
    expanded.reserve(text.size());
    for (std::size_t offset = 0u; offset < text.size();) {
        const wchar_t current = text[offset];
        if (current >= kFirstGlyph && current <= kLastGlyph) return false;
        if (current == L'{' && offset + 1u < text.size() &&
            text[offset + 1u] == L'{') {
            expanded.push_back(L'{');
            offset += 2u;
            continue;
        }

        bool matched{};
        for (const Token& token : kTokens) {
            if (text.compare(offset, token.text.size(), token.text) == 0) {
                expanded.push_back(token.glyph);
                offset += token.text.size();
                matched = true;
                break;
            }
        }
        if (!matched) {
            expanded.push_back(current);
            ++offset;
        }
    }
    text = std::move(expanded);
    return true;
}

bool IsGlyph(wchar_t character) {
    return character >= kFirstGlyph && character <= kLastGlyph;
}

std::array<char, 2> EncodeGlyph(wchar_t character) {
    const unsigned index = static_cast<unsigned>(character - kFirstGlyph);
    return {static_cast<char>(0xF1u), static_cast<char>(0x40u + index)};
}

} // namespace novafix::compat::native_notification_text
