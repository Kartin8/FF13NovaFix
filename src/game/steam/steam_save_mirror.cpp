#include "game/steam/steam_save_mirror.h"

#include "diagnostics/log.h"
#include "game/steam/steam_save_checkpoints.h"
#include "game/steam/steam_save_files.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace novafix::game::steam_cloud::mirror {
namespace {

constexpr wchar_t kConflictDirectory[] = L"NovaFixCloudConflicts";
SRWLOCK g_saveWriteLock = SRWLOCK_INIT;

using save_files::AtomicWritePath;
using save_files::EnsureDirectory;
using save_files::FileTimeToUnixSeconds;
using save_files::PathFor;
using save_files::ReadWholeFile;

} // namespace

bool IsSafeSaveName(const char* name) {
    return save_files::IsSafeSaveName(name);
}

std::wstring Root() {
    return save_files::Root();
}

bool Write(const char* name, const void* data, std::int32_t size) {
    const std::wstring root = Root();
    if (!EnsureDirectory(root)) return false;
    const std::wstring path = PathFor(name);
    AcquireSRWLockExclusive(&g_saveWriteLock);
    const bool checkpointed =
        save_checkpoints::CaptureWhileMutationLocked(path);
    const bool written = AtomicWritePath(path, data, size, true);
    ReleaseSRWLockExclusive(&g_saveWriteLock);
    if (!checkpointed) {
        LogWarning(
            "Steam Cloud local save checkpoint failed: %s immediate-backup=enabled",
            name ? name : "<invalid>");
    }
    return written;
}

bool WriteCloudConflict(const char* name, const void* data,
                        std::int32_t size,
                        std::int64_t remoteTimestamp) {
    if (!IsSafeSaveName(name) || size <= 0 || !data) {
        return false;
    }
    std::wstring directory = Root();
    if (directory.empty()) return false;
    directory.push_back(L'\\');
    directory.append(kConflictDirectory);
    if (!EnsureDirectory(directory)) return false;

    std::wstring filename;
    for (const unsigned char character : std::string(name)) {
        filename.push_back(static_cast<wchar_t>(character));
    }
    filename.append(L".remote-");
    FILETIME capturedAt{};
    GetSystemTimeAsFileTime(&capturedAt);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = capturedAt.dwLowDateTime;
    ticks.HighPart = capturedAt.dwHighDateTime;
    if (remoteTimestamp > 0) {
        filename.append(std::to_wstring(remoteTimestamp));
        filename.push_back(L'-');
        filename.append(std::to_wstring(ticks.QuadPart));
    } else {
        filename.append(L"unknown-");
        filename.append(std::to_wstring(ticks.QuadPart));
    }
    filename.append(L".dat");

    AcquireSRWLockExclusive(&g_saveWriteLock);
    const bool written = AtomicWritePath(
        directory + L'\\' + filename, data, size, false);
    ReleaseSRWLockExclusive(&g_saveWriteLock);
    return written;
}

bool ImportCloudSaveIfMissing(const char* name, const void* data,
                              std::int32_t size,
                              std::int64_t remoteTimestamp) {
    if (!IsSafeSaveName(name) || size <= 0 || !data ||
        remoteTimestamp <= 0) {
        return false;
    }
    const std::wstring root = Root();
    if (!EnsureDirectory(root)) return false;
    const std::wstring path = PathFor(name);

    AcquireSRWLockExclusive(&g_saveWriteLock);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        ReleaseSRWLockExclusive(&g_saveWriteLock);
        return false;
    }
    const bool written = AtomicWritePath(path, data, size, false);
    bool timestamped = false;
    if (written) {
        HANDLE file = CreateFileW(
            path.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            constexpr std::uint64_t kEpoch = 116444736000000000ULL;
            ULARGE_INTEGER value{};
            value.QuadPart = kEpoch +
                static_cast<std::uint64_t>(remoteTimestamp) * 10000000ULL;
            FILETIME time{value.LowPart, value.HighPart};
            timestamped = SetFileTime(
                              file, nullptr, nullptr, &time) != FALSE;
            CloseHandle(file);
        }
    }
    ReleaseSRWLockExclusive(&g_saveWriteLock);
    if (written && !timestamped) {
        LogWarning(
            "Steam Cloud imported save kept its safe local timestamp: remote timestamp unavailable for %s",
            name);
    }
    return written;
}

ConditionalWriteResult ReplaceIfUnchanged(
    const char* name,
    const std::vector<unsigned char>& expected,
    const std::vector<unsigned char>& replacement) {
    if (!IsSafeSaveName(name) || expected.empty() || replacement.empty() ||
        expected.size() > static_cast<std::size_t>(
            save_files::kMaximumSaveBytes) ||
        replacement.size() > static_cast<std::size_t>(
            save_files::kMaximumSaveBytes)) {
        return ConditionalWriteResult::Failed;
    }

    const std::wstring path = PathFor(name);
    AcquireSRWLockExclusive(&g_saveWriteLock);
    std::vector<char> current;
    const bool read = ReadWholeFile(path, current);
    const bool unchanged = read && current.size() == expected.size() &&
        std::equal(current.begin(), current.end(), expected.begin());
    if (!unchanged) {
        ReleaseSRWLockExclusive(&g_saveWriteLock);
        return read ? ConditionalWriteResult::Changed
                    : ConditionalWriteResult::Failed;
    }

    const bool checkpointed =
        save_checkpoints::CaptureWhileMutationLocked(path);
    if (!checkpointed) {
        ReleaseSRWLockExclusive(&g_saveWriteLock);
        LogWarning(
            "Steam Cloud remote import blocked: local checkpoint could not be created for %s",
            name);
        return ConditionalWriteResult::Failed;
    }
    const bool written = AtomicWritePath(
        path, replacement.data(),
        static_cast<std::int32_t>(replacement.size()), true);
    ReleaseSRWLockExclusive(&g_saveWriteLock);
    return written ? ConditionalWriteResult::Written
                   : ConditionalWriteResult::Failed;
}

bool ReadBytes(const char* name, std::vector<unsigned char>& bytes) {
    std::vector<char> source;
    if (!ReadWholeFile(PathFor(name), source)) return false;
    bytes.assign(source.begin(), source.end());
    return true;
}

std::int32_t Read(const char* name, void* data, std::int32_t capacity) {
    if (!data || capacity < 0) return 0;
    std::vector<char> bytes;
    if (!ReadWholeFile(PathFor(name), bytes) || bytes.size() > static_cast<std::size_t>(capacity)) return 0;
    if (!bytes.empty()) std::memcpy(data, bytes.data(), bytes.size());
    return static_cast<std::int32_t>(bytes.size());
}

bool Remove(const char* name) {
    const std::wstring path = PathFor(name);
    if (path.empty()) return false;
    AcquireSRWLockExclusive(&g_saveWriteLock);
    const bool checkpointed =
        save_checkpoints::CaptureWhileMutationLocked(path);
    const bool removed = DeleteFileW(path.c_str()) != FALSE ||
                         GetLastError() == ERROR_FILE_NOT_FOUND;
    ReleaseSRWLockExclusive(&g_saveWriteLock);
    if (!checkpointed) {
        LogWarning(
            "Steam Cloud local delete checkpoint failed: %s deletion=requested",
            name ? name : "<invalid>");
    }
    return removed;
}

bool Exists(const char* name) {
    const DWORD attributes = GetFileAttributesW(PathFor(name).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::int32_t Size(const char* name) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(PathFor(name).c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER length{};
    length.LowPart = data.nFileSizeLow;
    length.HighPart = data.nFileSizeHigh;
    return length.QuadPart <= static_cast<std::uint64_t>(INT_MAX)
        ? static_cast<std::int32_t>(length.QuadPart) : 0;
}

std::int64_t Timestamp(const char* name) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(PathFor(name).c_str(), GetFileExInfoStandard, &data)) return 0;
    return FileTimeToUnixSeconds(data.ftLastWriteTime);
}

std::vector<FileInfo> Enumerate() {
    std::vector<FileInfo> files;
    const std::wstring pattern = Root() + L"\\*.dat";
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return files;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name;
        for (const wchar_t c : std::wstring(entry.cFileName)) {
            if (c > 0x7f) { name.clear(); break; }
            name.push_back(static_cast<char>(c));
        }
        if (!IsSafeSaveName(name.c_str())) continue;
        ULARGE_INTEGER length{};
        length.LowPart = entry.nFileSizeLow;
        length.HighPart = entry.nFileSizeHigh;
        if (length.QuadPart > static_cast<std::uint64_t>(INT_MAX)) continue;
        files.push_back({name, static_cast<std::int32_t>(length.QuadPart)});
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    std::sort(files.begin(), files.end(), [](const FileInfo& left, const FileInfo& right) {
        return _stricmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    return files;
}

} // namespace novafix::game::steam_cloud::mirror
