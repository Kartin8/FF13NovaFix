#pragma once

#include "game/titles/lightning_returns/ui/text_editor/native_api.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace novafix::game::lr_text_editor::detail {

std::size_t UnicodeCharacterCount(std::wstring_view text);
void TrimToMaximum(std::wstring& text, std::size_t maximumCharacters);
bool WideToUtf8(std::wstring_view source, std::string& result);
bool Utf8ToWide(std::string_view source, std::wstring& result);
bool GameTextToWideString(
    const NativeApi& api, const char* source, std::wstring& result);
bool BuildNativeText(
    const NativeApi& api, std::wstring_view source,
    std::vector<char>& text);
bool Utf8ToGameText(
    const NativeApi& api, std::string_view source, std::string& text);

} // namespace novafix::game::lr_text_editor::detail
