#include "input/prompt_policy.h"

namespace novafix::input {

NativePromptMode ResolvePromptMode(settings::PromptMode preference,
                                   InputSource source) {
    if (preference == settings::PromptMode::Controller) {
        return NativePromptMode::Controller;
    }
    if (preference == settings::PromptMode::Keyboard) {
        return NativePromptMode::Keyboard;
    }
    if (source == InputSource::Controller) return NativePromptMode::Controller;
    if (source == InputSource::KeyboardMouse) return NativePromptMode::Keyboard;
    return NativePromptMode::Unknown;
}

} // namespace novafix::input
