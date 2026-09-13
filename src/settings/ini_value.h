#pragma once

#include <span>
#include <string>

namespace novafix::settings::ini {

struct Entry {
    const wchar_t* key{};
    std::wstring value;
};

unsigned ReadUnsigned(const std::wstring& path, const wchar_t* section,
                      const wchar_t* key, unsigned fallback);
bool ReadBoolean(const std::wstring& path, const wchar_t* section,
                 const wchar_t* key, bool fallback);
unsigned ReadChoice(
    const std::wstring& path, const wchar_t* section,
    const wchar_t* key, std::span<const wchar_t* const> names,
    unsigned fallback);
float ReadFloat(const std::wstring& path, const wchar_t* section,
                const wchar_t* key, float fallback);
std::wstring FormatChoice(
    unsigned value, std::span<const wchar_t* const> names);
std::wstring FormatBoolean(bool value);
bool WriteValues(const std::wstring& path, const wchar_t* section,
                 std::span<const Entry> entries);

} // namespace novafix::settings::ini
