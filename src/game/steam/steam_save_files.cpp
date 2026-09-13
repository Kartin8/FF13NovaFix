#include "game/steam/steam_save_files.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <limits>

namespace novafix::game::steam_cloud::save_files {
namespace {

constexpr wchar_t kRelativeRoot[] = L"SquareEnix\\FinalFantasyXIII-2\\save";
std::atomic_uint64_t g_temporarySequence{};

bool WriteAll(HANDLE file, const void* data, std::int32_t size) {
    const auto* cursor = static_cast<const unsigned char*>(data);
    DWORD remaining = static_cast<DWORD>(size);
    while (remaining != 0) {
        DWORD written{};
        if (!WriteFile(file, cursor, remaining, &written, nullptr) ||
            written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

} // namespace

bool IsSafeSaveName(const char* name) {
    if (!name || !*name) return false;
    const std::size_t length = std::strlen(name);
    if (length > 127 || std::strstr(name, "..")) return false;
    for (const unsigned char c : std::string(name)) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    const char* dot = std::strrchr(name, '.');
    return dot && _stricmp(dot, ".dat") == 0;
}

std::wstring Root() {
    std::array<wchar_t, 32768> base{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", base.data(), static_cast<DWORD>(base.size()));
    if (!length || length >= base.size()) return {};
    std::wstring root(base.data(), length);
    root.push_back(L'\\');
    root.append(kRelativeRoot);
    return root;
}

std::wstring PathFor(const char* name) {
    if (!IsSafeSaveName(name)) return {};
    std::wstring path = Root();
    if (path.empty()) return {};
    path.push_back(L'\\');
    for (const unsigned char c : std::string(name)) {
        path.push_back(static_cast<wchar_t>(c));
    }
    return path;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring current;
    current.reserve(path.size());
    for (const wchar_t character : path) {
        current.push_back(character);
        if (character != L'\\' && character != L'/') continue;
        if (current.size() <= 3) continue;
        CreateDirectoryW(current.c_str(), nullptr);
    }
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

bool AtomicWritePath(const std::wstring& path, const void* data,
                     std::int32_t size, bool preservePrevious) {
    if (path.empty() || size < 0 || size > kMaximumSaveBytes ||
        (size != 0 && !data)) {
        return false;
    }
    wchar_t suffix[96]{};
    _snwprintf_s(
        suffix, std::size(suffix), _TRUNCATE,
        L".novafix.%08lX.%08lX.%llu.tmp", GetCurrentProcessId(),
        GetCurrentThreadId(),
        static_cast<unsigned long long>(
            g_temporarySequence.fetch_add(1)));
    const std::wstring temporary = path + suffix;
    HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = WriteAll(file, data, size);
    const bool flushed = written && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    bool replaced = false;
    if (flushed && preservePrevious &&
        GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring backup = path + L".novafix.bak";
        replaced = ReplaceFileW(
                       path.c_str(), temporary.c_str(), backup.c_str(),
                       REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
    }
    if (flushed && !replaced) {
        replaced = MoveFileExW(
                       temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING |
                           MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!replaced) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::wstring& path, std::vector<char>& bytes,
                   std::int32_t maximumBytes) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 ||
        length.QuadPart > maximumBytes) {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(length.QuadPart));
    std::size_t position{};
    bool ok = true;
    while (position < bytes.size()) {
        DWORD read{};
        const DWORD requested = static_cast<DWORD>((std::min<std::size_t>)(
            bytes.size() - position,
            std::numeric_limits<DWORD>::max()));
        if (!ReadFile(
                file, bytes.data() + position, requested, &read, nullptr) ||
            read == 0) {
            ok = false;
            break;
        }
        position += read;
    }
    CloseHandle(file);
    return ok;
}

std::int64_t FileTimeToUnixSeconds(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    constexpr std::uint64_t kEpoch = 116444736000000000ULL;
    return value.QuadPart > kEpoch
        ? static_cast<std::int64_t>(
              (value.QuadPart - kEpoch) / 10000000ULL)
        : 0;
}

} // namespace novafix::game::steam_cloud::save_files
