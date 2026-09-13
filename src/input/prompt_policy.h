#pragma once

#include "input/activity.h"
#include "settings/input_settings.h"

#include <cstdint>

namespace novafix::input {

enum class NativePromptMode : int {
    Controller = 0,
    Keyboard = 1,
    Unknown = -1,
};

inline constexpr std::uint64_t kAutomaticPromptSwitchDebounceMs = 42u;

NativePromptMode ResolvePromptMode(settings::PromptMode preference,
                                   InputSource source);

} // namespace novafix::input
