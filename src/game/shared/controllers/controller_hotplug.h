#pragma once

namespace novafix::game::controller_hotplug {

void Initialize();
bool Available();
void Pump();
void NotifyDeviceTopologyChanged();

} // namespace novafix::game::controller_hotplug
