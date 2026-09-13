#pragma once

#include <cstdint>

namespace novafix::game::steam_cloud::remote_storage::catalog {

std::int32_t Count();
const char* NameAndSize(std::int32_t index, std::int32_t* size);

} // namespace novafix::game::steam_cloud::remote_storage::catalog
