#pragma once

namespace novafix::game::xiii2_resource_budget {

void Initialize();

// Scales the native 15-ms model-initialization slice for the active frame target
void Pump();

} // namespace novafix::game::xiii2_resource_budget
