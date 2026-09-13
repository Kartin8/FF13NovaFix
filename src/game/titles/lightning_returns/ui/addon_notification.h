#pragma once

#include <string>

namespace novafix::game::lr_addon_notification {

void Install();
void Queue(std::wstring message, std::wstring signature);
bool HasPending();

} // namespace novafix::game::lr_addon_notification
