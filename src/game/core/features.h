#pragma once

namespace novafix::game::features {

// Device creation signals runtime readiness; Pump also activates features
// for nonstandard device creation paths
void Activate();

void Pump();

void NotifyDeviceTopologyChanged();

} // namespace novafix::game::features
