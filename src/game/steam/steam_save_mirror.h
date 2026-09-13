#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace novafix::game::steam_cloud::mirror {

struct FileInfo {
    std::string name;
    std::int32_t size{};
};

enum class ConditionalWriteResult {
    Written,
    Changed,
    Failed,
};

bool IsSafeSaveName(const char* name);
bool Write(const char* name, const void* data, std::int32_t size);
bool WriteCloudConflict(const char* name, const void* data,
                        std::int32_t size, std::int64_t remoteTimestamp);
bool ImportCloudSaveIfMissing(const char* name, const void* data,
                              std::int32_t size,
                              std::int64_t remoteTimestamp);
ConditionalWriteResult ReplaceIfUnchanged(
    const char* name,
    const std::vector<unsigned char>& expected,
    const std::vector<unsigned char>& replacement);
bool ReadBytes(const char* name, std::vector<unsigned char>& bytes);
std::int32_t Read(const char* name, void* data, std::int32_t capacity);
bool Remove(const char* name);
bool Exists(const char* name);
std::int32_t Size(const char* name);
std::int64_t Timestamp(const char* name);
std::vector<FileInfo> Enumerate();
std::wstring Root();

} // namespace novafix::game::steam_cloud::mirror
