#pragma once

#include "display/aspect_ratio.h"

#include <cstdint>

namespace novafix::game {
class ImageView;
}

namespace novafix::game::ff13_ui::battle_hud_layout {

class ScopedProduct {
public:
    ScopedProduct(void* product, const display::AspectRatio& aspect,
                  std::uintptr_t imageBase, void* battleResource);
    ~ScopedProduct();
    ScopedProduct(const ScopedProduct&) = delete;
    ScopedProduct& operator=(const ScopedProduct&) = delete;

private:
    void* previousRoot_{};
    float previousOffset_{};
};

bool Affects(void* node);
void Apply(void* node);
void Prepare(const ImageView& image);
bool InstallDetour(const ImageView& image);
void Revert();

} // namespace novafix::game::ff13_ui::battle_hud_layout
