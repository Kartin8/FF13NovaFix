#pragma once

namespace novafix::game::ff13_prompt_mode {

void Pump();

// Restores the native default-pool controller-atlas backup from a persistent
// system-memory snapshot after a D3D9 reset
void BeforeDeviceReset();

} // namespace novafix::game::ff13_prompt_mode
