#pragma once

namespace novafix::game::xiii2_shadow_fixed_depth {

// Owns the two exact Crystal Tools draw boundaries needed by the accepted
// fixed-depth shadow producer and the exact native atlas-copy boundary. The
// producer is enabled only when all three contracts are installed together
void Pump();
bool Available();
bool Enable();
bool Enabled();

} // namespace novafix::game::xiii2_shadow_fixed_depth
