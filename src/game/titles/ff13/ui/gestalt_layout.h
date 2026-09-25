#pragma once

#include "display/aspect_ratio.h"

#include <cstdint>

namespace novafix::game::ff13_ui::gestalt_layout {

class ScopedProduct {
public:
    ScopedProduct(void* product, const display::AspectRatio& aspect,
                  std::uintptr_t imageBase);
    ~ScopedProduct();
    ScopedProduct(const ScopedProduct&) = delete;
    ScopedProduct& operator=(const ScopedProduct&) = delete;

private:
    void* previousRoot_{};
    float previousX_{};
    float previousY_{};
};

bool Affects(void* node);
void Apply(void* node);

} // namespace novafix::game::ff13_ui::gestalt_layout
