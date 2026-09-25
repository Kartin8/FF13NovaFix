#pragma once

namespace novafix::game {
class ImageView;
}

namespace novafix::game::ff13_ui::map_alert_vfx {

// The alert is a separate VFX, not part of GuiNaviMap's retained tree.
// Hook its local owner call after aspect-ratio support is installed.
void Install(const ImageView& image);

} // namespace novafix::game::ff13_ui::map_alert_vfx
