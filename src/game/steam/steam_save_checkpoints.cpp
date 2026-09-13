#include "game/steam/steam_save_checkpoints.h"

#include <windows.h>

#include <algorithm>
#include <vector>

namespace novafix::game::steam_cloud::save_checkpoints {
namespace {

constexpr unsigned kSessionCheckpointCount = 5u;
std::vector<std::wstring> g_sessionCheckpointedPaths;

std::wstring CheckpointPath(const std::wstring& path, unsigned index) {
    return path + L".novafix.restore." + std::to_wstring(index);
}

bool MoveIfPresent(const std::wstring& source,
                   const std::wstring& destination) {
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    return MoveFileExW(
               source.c_str(), destination.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

} // namespace

bool CaptureWhileMutationLocked(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    if (std::find(g_sessionCheckpointedPaths.begin(),
                  g_sessionCheckpointedPaths.end(), path) !=
        g_sessionCheckpointedPaths.end()) {
        return true;
    }
    bool rotated = true;
    for (unsigned index = kSessionCheckpointCount - 1u;
         index > 0u; --index) {
        rotated = MoveIfPresent(
                      CheckpointPath(path, index - 1u),
                      CheckpointPath(path, index)) &&
                  rotated;
    }

    const std::wstring checkpoint = CheckpointPath(path, 0u);
    bool captured = CreateHardLinkW(
                        checkpoint.c_str(), path.c_str(), nullptr) != FALSE;
    if (!captured) {
        captured = CopyFileW(path.c_str(), checkpoint.c_str(), FALSE) != FALSE;
    }
    if (captured) g_sessionCheckpointedPaths.push_back(path);
    return rotated && captured;
}

} // namespace novafix::game::steam_cloud::save_checkpoints
