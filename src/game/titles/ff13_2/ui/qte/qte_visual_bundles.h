#pragma once

#include "input/prompt_policy.h"

namespace novafix::game {

class ImageView;

namespace xiii2_qte_visuals {

enum class SwitchResult {
    NoOwners,
    Unchanged,
    Switched,
    Deferred,
    Unsupported,
};

bool HasExpectedContract(const ImageView& image);
bool Install(const ImageView& image);
bool Revert();

void RegisterOwner(void* owner, input::NativePromptMode mode);
void UnregisterOwner(void* owner);
SwitchResult SwitchAll(input::NativePromptMode mode);

} // namespace xiii2_qte_visuals
} // namespace novafix::game
