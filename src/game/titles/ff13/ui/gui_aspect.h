#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::patch {
class Transaction;
}

namespace novafix::game::ff13_ui::gui_aspect {

bool RegisterMinimapAspectCalls(patch::Transaction& calls,
                                const ImageView& image);
bool RegisterMinimapCameraCalls(patch::Transaction& calls,
                                const ImageView& image);
bool RegisterProjectionCalls(patch::Transaction& calls,
                             const ImageView& image);
void Prepare(const ImageView& image);
bool InstallDetours(const ImageView& image);
void Revert();

} // namespace novafix::game::ff13_ui::gui_aspect
