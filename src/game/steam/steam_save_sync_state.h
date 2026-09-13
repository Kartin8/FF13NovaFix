#pragma once

#include "game/steam/steam_save_identity.h"

#include <string>
#include <vector>

namespace novafix::game::steam_cloud::sync_state {

struct Entry {
    std::string name;
    save_identity::ContentIdentity identity;
};

bool Load(std::vector<Entry>& entries);
bool Store(const std::vector<Entry>& entries);

} // namespace novafix::game::steam_cloud::sync_state
