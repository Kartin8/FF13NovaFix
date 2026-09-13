#pragma once

#include "input/prompt_policy.h"

namespace novafix::game {

class ImageView;

namespace xiii2_qte_prompts {

using ReadPromptModeFn = input::NativePromptMode (*)();
using ApplyPromptModeFn = void (*)(input::NativePromptMode mode);

struct NativeFunctions {
    ReadPromptModeFn readRequestedMode{};
    ReadPromptModeFn readAppliedMode{};
    ApplyPromptModeFn applyMode{};
};

bool HasExpectedContract(const ImageView& image);
bool InstallLifecycle(const ImageView& image,
                      const NativeFunctions& functions);

// Freeze the epoch if any live owner cannot join an all-owner switch
input::NativePromptMode ResolveMode(input::NativePromptMode requested);

// Game-thread only: switch every validated live QTE bundle together
input::NativePromptMode ApplyRequestedMode(
    input::NativePromptMode requested);

// Apply coalesced cinema bindings after the texture-load batch returns
void FlushPendingAtlas();

} // namespace xiii2_qte_prompts
} // namespace novafix::game
