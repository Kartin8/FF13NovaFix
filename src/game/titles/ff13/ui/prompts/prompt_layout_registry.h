#pragma once

#include <cstddef>

namespace novafix::game {
class ImageView;
}

namespace novafix::game::ff13_prompt_layout_registry {

struct RefreshResult {
    std::size_t layoutOwned{};
    std::size_t direct{};
};

bool Install(const ImageView& image);
bool InstallLifetimeObserver();
bool Revert();
RefreshResult Refresh();

} // namespace novafix::game::ff13_prompt_layout_registry
