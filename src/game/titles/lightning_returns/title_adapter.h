#pragma once

#include "game/core/catalog.h"

namespace novafix::game::adapters::lightning_returns {

std::span<const BuildDescriptor> Builds();
std::span<const FeatureDescriptor> Features();

} // namespace novafix::game::adapters::lightning_returns
