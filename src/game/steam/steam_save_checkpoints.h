#pragma once

#include <string>

namespace novafix::game::steam_cloud::save_checkpoints {

// The caller holds the save-mutation lock shared with writes, deletes,
// conflict archives, and cloud imports
bool CaptureWhileMutationLocked(const std::wstring& path);

} // namespace novafix::game::steam_cloud::save_checkpoints
