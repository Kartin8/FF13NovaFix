#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace novafix::game::steam_cloud::achievement_queue {

void LoadOnce();
void Queue(std::string_view name);
std::vector<std::string> Snapshot();
void Complete(const std::vector<std::string>& names);
void MarkDirty(std::string_view name);
bool IsDirty(std::string_view name);
bool HasPending();

} // namespace novafix::game::steam_cloud::achievement_queue
