#pragma once

namespace novafix::settings {

enum class InterfaceLanguage : unsigned {
    English,
    Japanese,
    German,
    French,
    Italian,
    Spanish,
    Korean,
    TraditionalChinese,
};

enum class VoiceLanguage : unsigned {
    English,
    Japanese,
};

struct GameSettings {
    bool overrideLanguageSettings{};
    InterfaceLanguage interfaceLanguage{InterfaceLanguage::English};
    VoiceLanguage voiceLanguage{VoiceLanguage::English};
};

GameSettings ActiveGame();
GameSettings StoredGame();
bool SaveGame(const GameSettings& value);
bool SaveLanguageOverride(bool enabled);
bool SaveInterfaceLanguage(InterfaceLanguage value);
bool SaveVoiceLanguage(VoiceLanguage value);
bool OverridesGameLanguage();

} // namespace novafix::settings
