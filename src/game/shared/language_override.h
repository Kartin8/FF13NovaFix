#pragma once

#include "game/core/game_profile.h"
#include "settings/game_settings.h"

#include <optional>

namespace novafix::game::language_override {

void InstallEarly(Title title);
std::optional<settings::InterfaceLanguage> CurrentInterfaceLanguage(
    Title title);
std::optional<settings::VoiceLanguage> CurrentVoiceLanguage(Title title);
bool ApplyVoiceLanguage(Title title, settings::VoiceLanguage language);
void Report();

} // namespace novafix::game::language_override
