#pragma once

namespace novafix::game::ff13_motion_blend_correction {

// Restores smooth XIII animation-state transitions with the runtime-validated
// 200-ms blend and 2.5% non-zero floor
void Pump();

} // namespace novafix::game::ff13_motion_blend_correction
