#pragma once

#include <cstdint>

namespace novafix::game::steam_cloud::remote_storage {

using SteamApiCall = std::uint64_t;
using UGCFileWriteStreamHandle = std::uint64_t;

struct Interface {
    virtual bool FileWrite(const char*, const void*, std::int32_t) = 0;
    virtual std::int32_t FileRead(const char*, void*, std::int32_t) = 0;
    virtual bool FileForget(const char*) = 0;
    virtual bool FileDelete(const char*) = 0;
    virtual SteamApiCall FileShare(const char*) = 0;
    virtual bool SetSyncPlatforms(const char*, std::uint32_t) = 0;
    virtual UGCFileWriteStreamHandle FileWriteStreamOpen(const char*) = 0;
    virtual bool FileWriteStreamWriteChunk(
        UGCFileWriteStreamHandle, const void*, std::int32_t) = 0;
    virtual bool FileWriteStreamClose(UGCFileWriteStreamHandle) = 0;
    virtual bool FileWriteStreamCancel(UGCFileWriteStreamHandle) = 0;
    virtual bool FileExists(const char*) = 0;
    virtual bool FilePersisted(const char*) = 0;
    virtual std::int32_t GetFileSize(const char*) = 0;
    virtual std::int64_t GetFileTimestamp(const char*) = 0;
    virtual std::uint32_t GetSyncPlatforms(const char*) = 0;
    virtual std::int32_t GetFileCount() = 0;
    virtual const char* GetFileNameAndSize(std::int32_t, std::int32_t*) = 0;
    virtual bool GetQuota(std::int32_t*, std::int32_t*) = 0;
    virtual bool IsCloudEnabledForAccount() = 0;
    virtual bool IsCloudEnabledForApp() = 0;
};

static_assert(sizeof(void*) == 4,
              "The XIII-2 Steam bridge ABI is 32-bit only.");

} // namespace novafix::game::steam_cloud::remote_storage
