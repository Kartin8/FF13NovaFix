#pragma once

namespace novafix::game::controller_hotplug::backend {

bool Initialize();

// Requests a reconnect after a real device-topology change. The request is
// always executed by the title's native input path, never by the PnP callback
void NotifyTopologyChanged();
void Pump();

bool RequiresXInputVerification();

} // namespace novafix::game::controller_hotplug::backend
