#include "game/core/catalog.h"

#include "game/titles/ff13/title_adapter.h"
#include "game/titles/ff13_2/title_adapter.h"
#include "game/titles/lightning_returns/title_adapter.h"

namespace novafix::game::adapters {

std::span<const BuildDescriptor> BuildsFor(Title title) {
    switch (title) {
    case Title::FinalFantasyXIII: return ff13::Builds();
    case Title::FinalFantasyXIII2: return ff13_2::Builds();
    case Title::LightningReturns: return lightning_returns::Builds();
    case Title::Unknown: return {};
    }
    return {};
}

std::span<const FeatureDescriptor> FeaturesFor(Title title) {
    switch (title) {
    case Title::FinalFantasyXIII: return ff13::Features();
    case Title::FinalFantasyXIII2: return ff13_2::Features();
    case Title::LightningReturns: return lightning_returns::Features();
    case Title::Unknown: return {};
    }
    return {};
}

} // namespace novafix::game::adapters
