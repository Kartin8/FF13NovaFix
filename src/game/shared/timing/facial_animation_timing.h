#pragma once

namespace novafix::game::facial_animation_timing {

void Initialize();

// Updates the measured render interval. The installed hook remains a no-op at
// 30 FPS or when the setting is disabled
void Pump();

} // namespace novafix::game::facial_animation_timing
