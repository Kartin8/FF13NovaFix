#pragma once

namespace novafix::patch::integrity_monitor {

// Verify patch ownership outside Present so another hook can't mask it
void Start();

void PumpFallback();

} // namespace novafix::patch::integrity_monitor
