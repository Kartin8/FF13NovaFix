#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::patch {
class Transaction;
}

namespace novafix::game::xiii2_aspect_ratio {
struct MinimapClipCorrection;
}

namespace novafix::game::xiii2_ui::gui_aspect {

bool MatchesContract(const ImageView& image);
void Prepare(const ImageView& image);
void PrepareNativeAspect(const ImageView& image);
void ResetPreparedState();
bool RegisterMinimapAspectCalls(patch::Transaction& calls,
                                const ImageView& image);
bool RegisterMinimapMatrixCall(patch::Transaction& calls,
                               const ImageView& image);
bool InstallDetours(const ImageView& image);
void RevertMinimapDraw();
void RevertGui();
xiii2_aspect_ratio::MinimapClipCorrection CurrentMinimapClipCorrection();

} // namespace novafix::game::xiii2_ui::gui_aspect
