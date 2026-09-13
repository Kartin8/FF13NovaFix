#include "settings/ini_value.h"

#include <windows.h>

#include <cerrno>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <limits>
#include <vector>

namespace novafix::settings::ini {
namespace {

SRWLOCK g_writeLock = SRWLOCK_INIT;

bool ParseUnsigned(const wchar_t* text, unsigned* output) {
    if (!text || !output) return false;
    while (std::iswspace(*text)) ++text;
    if (!*text || *text == L'-' || *text == L'+') return false;

    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(text, &end, 10);
    if (end == text || errno == ERANGE ||
        parsed > std::numeric_limits<unsigned>::max()) {
        return false;
    }
    while (end && std::iswspace(*end)) ++end;
    if (!end || *end != L'\0') return false;
    *output = static_cast<unsigned>(parsed);
    return true;
}

bool ReadText(const std::wstring& path, const wchar_t* section,
              const wchar_t* key, wchar_t (&text)[64]) {
    if (path.empty() || !section || !key) return false;
    const DWORD length = GetPrivateProfileStringW(
        section, key, L"", text, static_cast<DWORD>(std::size(text)),
        path.c_str());
    return length != 0 && length < std::size(text) - 1;
}

} // namespace

unsigned ReadUnsigned(const std::wstring& path, const wchar_t* section,
                      const wchar_t* key, unsigned fallback) {
    wchar_t text[64]{};
    if (!ReadText(path, section, key, text)) return fallback;
    unsigned value{};
    return ParseUnsigned(text, &value) ? value : fallback;
}

bool ReadBoolean(const std::wstring& path, const wchar_t* section,
                 const wchar_t* key, bool fallback) {
    wchar_t text[64]{};
    if (!ReadText(path, section, key, text)) return fallback;
    if (_wcsicmp(text, L"Off") == 0) return false;
    if (_wcsicmp(text, L"On") == 0) return true;

    // Temporary compatibility with numeric values written by older releases
    unsigned numeric{};
    if (ParseUnsigned(text, &numeric) && numeric <= 1u) {
        return numeric != 0u;
    }
    return fallback;
}

unsigned ReadChoice(
    const std::wstring& path, const wchar_t* section,
    const wchar_t* key, std::span<const wchar_t* const> names,
    unsigned fallback) {
    wchar_t text[64]{};
    if (!ReadText(path, section, key, text)) return fallback;
    for (unsigned index = 0; index < names.size(); ++index) {
        if (_wcsicmp(text, names[index]) == 0) return index;
    }

    // Temporary compatibility with numeric values written by older releases
    unsigned numeric{};
    if (!ParseUnsigned(text, &numeric)) return fallback;
    return numeric;
}

float ReadFloat(const std::wstring& path, const wchar_t* section,
                const wchar_t* key, float fallback) {
    wchar_t text[64]{};
    if (!ReadText(path, section, key, text)) return fallback;

    const wchar_t* begin = text;
    while (std::iswspace(*begin)) ++begin;
    errno = 0;
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(begin, &end);
    if (end == begin || errno == ERANGE || !std::isfinite(parsed)) {
        return fallback;
    }
    while (end && std::iswspace(*end)) ++end;
    return end && *end == L'\0' ? parsed : fallback;
}

std::wstring FormatChoice(
    unsigned value, std::span<const wchar_t* const> names) {
    if (value >= names.size()) return std::to_wstring(value);
    return names[value];
}

std::wstring FormatBoolean(bool value) {
    return value ? L"On" : L"Off";
}

bool WriteValues(const std::wstring& path, const wchar_t* section,
                 std::span<const Entry> entries) {
    if (path.empty() || !section || entries.empty()) return false;

    AcquireSRWLockExclusive(&g_writeLock);

    struct PreviousValue {
        bool existed{};
        std::wstring value;
    };
    constexpr wchar_t missing[] = L"\x1F";
    std::vector<PreviousValue> previous;
    previous.reserve(entries.size());
    for (const Entry& entry : entries) {
        wchar_t value[256]{};
        const DWORD length = entry.key
            ? GetPrivateProfileStringW(
                  section, entry.key, missing, value,
                  static_cast<DWORD>(std::size(value)), path.c_str())
            : 0u;
        const bool existed =
            entry.key && !(length == 1u && value[0] == missing[0]);
        previous.push_back({
            existed,
            existed ? std::wstring(value, length) : std::wstring{},
        });
    }

    std::size_t written = 0;
    for (; written < entries.size(); ++written) {
        const Entry& entry = entries[written];
        if (!entry.key ||
            !WritePrivateProfileStringW(
                section, entry.key, entry.value.c_str(), path.c_str())) {
            break;
        }
    }
    const bool saved = written == entries.size();
    if (!saved) {
        while (written > 0) {
            --written;
            const PreviousValue& value = previous[written];
            WritePrivateProfileStringW(
                section, entries[written].key,
                value.existed ? value.value.c_str() : nullptr,
                path.c_str());
        }
    }

    ReleaseSRWLockExclusive(&g_writeLock);
    return saved;
}

} // namespace novafix::settings::ini
