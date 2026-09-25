#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::patch {
class Transaction;
}

namespace novafix::game::ff13_ui::menu_layout {

bool RegisterProductUpdatePatch(patch::Transaction& calls,
                                const ImageView& image);
bool RegisterNodeTransformCall(patch::Transaction& calls,
                               const ImageView& image);
void Prepare(const ImageView& image);
void CorrectLocalCursor(void* widget, float* position);
bool HasColumnProfile(void* product);

} // namespace novafix::game::ff13_ui::menu_layout
