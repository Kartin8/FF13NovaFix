#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace novafix::game::steam_cloud::save_files {

constexpr std::int32_t kMaximumSaveBytes = 100 * 1024 * 1024;

bool IsSafeSaveName(const char* name);
std::wstring Root();
std::wstring PathFor(const char* name);
bool EnsureDirectory(const std::wstring& path);
bool AtomicWritePath(const std::wstring& path, const void* data,
                     std::int32_t size, bool preservePrevious);
bool ReadWholeFile(const std::wstring& path, std::vector<char>& bytes,
                   std::int32_t maximumBytes = kMaximumSaveBytes);
std::int64_t FileTimeToUnixSeconds(const FILETIME& time);

} // namespace novafix::game::steam_cloud::save_files
