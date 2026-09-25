#include "settings/game_settings.h"

#include "settings/config_path.h"
#include "settings/ini_value.h"
#include "settings/named_values.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace novafix::settings {
namespace {

INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK g_lock = SRWLOCK_INIT;
GameSettings g_settings{};
std::wstring g_configPath;

void Normalize(GameSettings& value) {
    value.interfaceLanguage = static_cast<InterfaceLanguage>(std::clamp(
        static_cast<unsigned>(value.interfaceLanguage), 0u, 7u));
    value.voiceLanguage = static_cast<VoiceLanguage>(std::clamp(
        static_cast<unsigned>(value.voiceLanguage), 0u, 1u));
}

BOOL CALLBACK Load(PINIT_ONCE, PVOID, PVOID*) {
    g_configPath = storage::ConfigFile();
    GameSettings loaded{};
    if (!g_configPath.empty() &&
        GetFileAttributesW(g_configPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        loaded.overrideLanguageSettings = ini::ReadBoolean(
            g_configPath, L"Game", L"OverrideLanguageSettings", false);
        const unsigned legacyInterface = ini::ReadChoice(
            g_configPath, L"Game", L"XIII2InterfaceLanguage",
            names::kInterfaceLanguage,
            static_cast<unsigned>(loaded.interfaceLanguage));
        loaded.interfaceLanguage = static_cast<InterfaceLanguage>(
            ini::ReadChoice(
                g_configPath, L"Game", L"InterfaceLanguage",
                names::kInterfaceLanguage, legacyInterface));
        const unsigned legacyVoice = ini::ReadChoice(
            g_configPath, L"Game", L"XIII2VoiceLanguage",
            names::kLegacyVoiceLanguage, 1u);
        loaded.voiceLanguage = static_cast<VoiceLanguage>(ini::ReadChoice(
            g_configPath, L"Game", L"VoiceLanguage",
            names::kVoiceLanguage, legacyVoice == 2u ? 1u : 0u));
    }
    Normalize(loaded);
    g_settings = loaded;
    return TRUE;
}

void EnsureLoaded() {
    InitOnceExecuteOnce(&g_loadOnce, &Load, nullptr, nullptr);
}

} // namespace

GameSettings ActiveGame() {
    return StoredGame();
}

GameSettings StoredGame() {
    EnsureLoaded();
    AcquireSRWLockShared(&g_lock);
    const GameSettings result = g_settings;
    ReleaseSRWLockShared(&g_lock);
    return result;
}

bool SaveGame(const GameSettings& requested) {
    EnsureLoaded();
    GameSettings value = requested;
    Normalize(value);
    const std::vector<ini::Entry> entries{
        {L"OverrideLanguageSettings",
         ini::FormatBoolean(value.overrideLanguageSettings)},
        {L"InterfaceLanguage", ini::FormatChoice(
             static_cast<unsigned>(value.interfaceLanguage),
             names::kInterfaceLanguage)},
        {L"VoiceLanguage", ini::FormatChoice(
             static_cast<unsigned>(value.voiceLanguage),
             names::kVoiceLanguage)},
    };
    if (g_configPath.empty() ||
        !ini::WriteValues(g_configPath, L"Game", entries)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_settings = value;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool SaveLanguageOverride(bool enabled) {
    EnsureLoaded();
    const ini::Entry entry{
        L"OverrideLanguageSettings", ini::FormatBoolean(enabled)};
    if (g_configPath.empty() ||
        !ini::WriteValues(
            g_configPath, L"Game",
            std::span<const ini::Entry>(&entry, 1u))) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_settings.overrideLanguageSettings = enabled;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool SaveInterfaceLanguage(InterfaceLanguage requested) {
    EnsureLoaded();
    GameSettings value = StoredGame();
    value.interfaceLanguage = requested;
    Normalize(value);
    const ini::Entry entry{
        L"InterfaceLanguage",
        ini::FormatChoice(static_cast<unsigned>(value.interfaceLanguage),
                          names::kInterfaceLanguage)};
    if (g_configPath.empty() ||
        !ini::WriteValues(
            g_configPath, L"Game",
            std::span<const ini::Entry>(&entry, 1u))) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_settings.interfaceLanguage = value.interfaceLanguage;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool SaveVoiceLanguage(VoiceLanguage requested) {
    EnsureLoaded();
    GameSettings value = StoredGame();
    value.voiceLanguage = requested;
    Normalize(value);
    const ini::Entry entry{
        L"VoiceLanguage",
        ini::FormatChoice(static_cast<unsigned>(value.voiceLanguage),
                          names::kVoiceLanguage)};
    if (g_configPath.empty() ||
        !ini::WriteValues(
            g_configPath, L"Game",
            std::span<const ini::Entry>(&entry, 1u))) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_settings.voiceLanguage = value.voiceLanguage;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool OverridesGameLanguage() {
    return ActiveGame().overrideLanguageSettings;
}

} // namespace novafix::settings
