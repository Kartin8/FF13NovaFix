#include "game/titles/lightning_returns/ui/text_editor/game_text_converter.h"

#include "patch/memory_access.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace novafix::game::lr_text_editor::detail {
namespace {

constexpr short kMissingGameGlyph = 0x25A1;

} // namespace

std::size_t UnicodeCharacterCount(std::wstring_view text) {
    std::size_t count = 0u;
    for (std::size_t index = 0u; index < text.size(); ++index, ++count) {
        if (IS_HIGH_SURROGATE(text[index]) && index + 1u < text.size() &&
            IS_LOW_SURROGATE(text[index + 1u])) {
            ++index;
        }
    }
    return count;
}

void TrimToMaximum(std::wstring& text, std::size_t maximumCharacters) {
    std::size_t end = 0u;
    for (std::size_t count = 0u;
         end < text.size() && count < maximumCharacters; ++count) {
        if (IS_HIGH_SURROGATE(text[end]) && end + 1u < text.size() &&
            IS_LOW_SURROGATE(text[end + 1u])) {
            ++end;
        }
        ++end;
    }
    text.resize(end);
}

bool WideToUtf8(std::wstring_view source, std::string& result) {
    result.clear();
    if (source.empty()) return true;
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
        static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    result.resize(static_cast<std::size_t>(bytes));
    return WideCharToMultiByte(
               CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
               static_cast<int>(source.size()), result.data(), bytes,
               nullptr, nullptr) == bytes;
}

bool Utf8ToWide(std::string_view source, std::wstring& result) {
    result.clear();
    if (source.empty()) return true;
    const int characters = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
        static_cast<int>(source.size()), nullptr, 0);
    if (characters <= 0) return false;
    result.resize(static_cast<std::size_t>(characters));
    return MultiByteToWideChar(
               CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
               static_cast<int>(source.size()), result.data(),
               characters) == characters;
}

bool GameTextToWideString(
    const NativeApi& api, const char* source, std::wstring& result) {
    result.clear();
    if (!source || !*source) return true;
    if (!api.gameTextToWide ||
        !patch::memory::IsReadable(source, 1u)) {
        return false;
    }
    const unsigned int required = api.gameTextToWide(nullptr, source);
    if (required > 4096u) return false;
    result.assign(static_cast<std::size_t>(required) + 1u, L'\0');
    const unsigned int converted = api.gameTextToWide(result.data(), source);
    if (converted > required) return false;
    result.resize(converted);
    return true;
}

bool BuildNativeText(
    const NativeApi& api, std::wstring_view source,
    std::vector<char>& text) {
    text.clear();
    if (!api.gameWideToText) return false;
    const std::wstring stable(source);
    const int bytes = api.gameWideToText(
        nullptr, stable.c_str(), kMissingGameGlyph);
    if (bytes < 0 || bytes > 8192) return false;
    text.resize(static_cast<std::size_t>(bytes) + 1u, '\0');
    return bytes == 0 ||
        api.gameWideToText(
            text.data(), stable.c_str(), kMissingGameGlyph) == bytes;
}

bool Utf8ToGameText(
    const NativeApi& api, std::string_view source, std::string& text) {
    text.clear();
    std::wstring wide;
    if (!Utf8ToWide(source, wide)) return false;

    std::vector<char> native;
    if (!BuildNativeText(api, wide, native) || native.empty()) return false;
    text.assign(native.data(), native.size() - 1u);
    return true;
}

} // namespace novafix::game::lr_text_editor::detail
