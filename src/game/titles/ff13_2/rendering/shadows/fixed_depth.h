#pragma once

namespace novafix::game::xiii2_shadow_fixed_depth {

// Hooks both fixed-depth draw sites and the native atlas copy
// Enable the producer only if all three hooks install successfully
void Pump();
bool Available();
bool Enable();
bool Enabled();

} // namespace novafix::game::xiii2_shadow_fixed_depth
