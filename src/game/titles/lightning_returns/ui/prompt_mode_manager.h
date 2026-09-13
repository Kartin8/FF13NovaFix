#pragma once

#include "input/prompt_policy.h"

namespace novafix::game::lr_prompts {

void Pump();
input::NativePromptMode CurrentMode();
bool UsesOppositeConfirmLayout();

} // namespace novafix::game::lr_prompts
