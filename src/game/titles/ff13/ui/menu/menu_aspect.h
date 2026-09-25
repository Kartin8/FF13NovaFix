#pragma once

namespace novafix::display {
struct ScissorRect;
}

namespace novafix::game {
class ImageView;
}

namespace novafix::patch {
class Transaction;
}

namespace novafix::game::ff13_ui::menu_aspect {

bool RegisterProductUpdatePatch(patch::Transaction& calls,
                                const ImageView& image);
bool RegisterNodeTransformCall(patch::Transaction& calls,
                               const ImageView& image);
bool RegisterListScissorCalls(patch::Transaction& calls,
                              const ImageView& image);
void Prepare(const ImageView& image);
bool InstallDetours(const ImageView& image);
void Revert();
bool ActiveGuiListScissorRect(const display::ScissorRect& source,
                              display::ScissorRect& output);

} // namespace novafix::game::ff13_ui::menu_aspect
