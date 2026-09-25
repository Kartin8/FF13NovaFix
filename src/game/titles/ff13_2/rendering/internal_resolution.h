#pragma once

namespace novafix {
class ImageView;
namespace patch { class Transaction; }
}

namespace novafix::game::xiii2_internal_resolution {

// Let the game's existing render-target override handle resolutions where
// only one dimension differs from its 1280x720 reference size.
bool RegisterPatches(patch::Transaction& transaction,
                     const ImageView& image);

} // namespace novafix::game::xiii2_internal_resolution
