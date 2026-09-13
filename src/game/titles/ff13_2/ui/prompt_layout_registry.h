#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::game::xiii2_prompt_layout_registry {

bool Install(const ImageView& image);
bool Revert();
void QueueVisibleReflow();
void CompleteVisibleReflow();

} // namespace novafix::game::xiii2_prompt_layout_registry
