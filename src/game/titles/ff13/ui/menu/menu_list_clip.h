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

namespace novafix::game::ff13_ui::menu_list_clip {

bool RegisterCalls(patch::Transaction& calls, const ImageView& image);
void Prepare(const ImageView& image);
bool InstallDetours(const ImageView& image);
void Revert();
bool ActiveRect(const display::ScissorRect& source,
                display::ScissorRect& output);

} // namespace novafix::game::ff13_ui::menu_list_clip
