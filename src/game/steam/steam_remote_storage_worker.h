#pragma once

#include "game/steam/steam_remote_storage_interface.h"

#include <cstdint>

namespace novafix::game::steam_cloud::remote_storage::worker {

void SetRemote(Interface* remote);
void ConfigureAsyncWrites(bool enabled);
bool RecordLocalWrite(const char* name, const void* data, std::int32_t size);
void RecordLocalDelete(const char* name);
bool IsDeletionPending(const char* name);
bool CloudEnabled();
bool WaitForInitialReconciliation(std::uint32_t milliseconds);
bool Stop(bool wait);

} // namespace novafix::game::steam_cloud::remote_storage::worker
