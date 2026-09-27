#pragma once

#include "display/aspect_runtime.h"
#include "display/scissor_transform.h"

namespace novafix::game::xiii2_crystarium_list {

void Initialize();
display::ScissorRect CorrectScissor(
    display::ScissorRect source, display::ScissorRect corrected,
    const display::AspectRatio& aspect);

} // namespace novafix::game::xiii2_crystarium_list
