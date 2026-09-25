#pragma once

#include <array>
#include <string>
#include <string_view>

namespace novafix::compat::native_notification_text {

inline constexpr wchar_t kFirstGlyph = 0xE100;
inline constexpr wchar_t kConfirmGlyph = kFirstGlyph + 0;
inline constexpr wchar_t kCancelGlyph = kFirstGlyph + 1;
inline constexpr wchar_t kFaceLeftGlyph = kFirstGlyph + 2;
inline constexpr wchar_t kFaceTopGlyph = kFirstGlyph + 3;
inline constexpr wchar_t kMenuGlyph = kFirstGlyph + 4;
inline constexpr wchar_t kLastGlyph = kMenuGlyph;

bool ExpandMarkup(std::wstring& text);
bool IsGlyph(wchar_t character);
std::array<char, 2> EncodeGlyph(wchar_t character);

} // namespace novafix::compat::native_notification_text
