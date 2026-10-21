#pragma once

#include "display/scissor_transform.h"

#include <array>
#include <cstdint>

namespace novafix::game::xiii2_menu_chrome {

struct VerticalAnchors {
    void* header{};
    std::array<void*, 2> footers{};
    void* historiaRoot{};
};

// Called at the native product-update boundary; pointers live for that update.
VerticalAnchors CaptureVerticalAnchors(void* product, std::uintptr_t imageBase);
bool IsHeader(void* node, const VerticalAnchors& anchors, std::uintptr_t imageBase);
display::ScissorRect CorrectScissor(
    display::ScissorRect source, display::ScissorRect corrected,
    const display::AspectRatio& aspect);

void Initialize();

} // namespace novafix::game::xiii2_menu_chrome
