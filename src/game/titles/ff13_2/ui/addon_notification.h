#pragma once

#include <string>

namespace novafix::game::xiii2_addon_notification {

void Initialize();
void Queue(std::wstring message, std::wstring signature);

} // namespace novafix::game::xiii2_addon_notification
