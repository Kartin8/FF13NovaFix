#pragma once

namespace novafix::game::xiii2_chain_timing {

void Initialize();

// Supplies real render delta above 30 FPS instead of ChainInstance's fixed 1/30 step
void Pump();

} // namespace novafix::game::xiii2_chain_timing
