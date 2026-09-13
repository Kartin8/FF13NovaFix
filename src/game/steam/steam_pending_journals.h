#pragma once

#include <string>
#include <vector>

namespace novafix::game::steam_cloud::pending_journals {

bool LoadAchievements(std::vector<std::string>& names);
bool StoreAchievements(const std::vector<std::string>& names);
bool LoadDeletions(std::vector<std::string>& names);
bool StoreDeletions(const std::vector<std::string>& names);
bool LoadUploads(std::vector<std::string>& names);
bool StoreUploads(const std::vector<std::string>& names);

} // namespace novafix::game::steam_cloud::pending_journals
