#pragma once

#include <cstdint>

namespace novafix::game::steam_cloud::remote_storage {

void* Wrap(void* original);
void ConfigureAsyncWrites(bool enabled);
bool StopWorker(bool wait);

} // namespace novafix::game::steam_cloud::remote_storage
