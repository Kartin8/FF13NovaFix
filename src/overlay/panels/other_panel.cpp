#include "overlay/panels/other_panel.h"

#include "game/core/game_profile.h"
#include "game/shared/language_override.h"
#include "overlay/panels/settings_widgets.h"
#include "overlay/ui_state.h"
#include "settings/game_settings.h"
#include "settings/input_settings.h"
#include "settings/performance_settings.h"

#include "imgui.h"

#include <algorithm>
#include <iterator>

namespace novafix::ui {
namespace {

void DrawGameLanguage() {
    const game::Title title = game::CurrentProfile().title;
    if ((title != game::Title::FinalFantasyXIII &&
         title != game::Title::FinalFantasyXIII2 &&
         title != game::Title::LightningReturns) ||
        !state::CollapsingHeader(
            "Game language", L"GeneralGameLanguage",
            ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (!widgets::BeginSettingsTable("##GameLanguageSettings")) return;

    settings::GameSettings value = settings::StoredGame();
    widgets::SettingRow(
        "Override launcher / Nova Chrysalia",
        "Off follows the language settings supplied by Steam, the game, or Nova Chrysalia");
    if (ImGui::Checkbox(
            "##OverrideLanguageSettings",
            &value.overrideLanguageSettings)) {
        if (value.overrideLanguageSettings) {
            value.interfaceLanguage =
                game::language_override::CurrentInterfaceLanguage(title)
                    .value_or(settings::InterfaceLanguage::English);
            value.voiceLanguage =
                game::language_override::CurrentVoiceLanguage(title)
                    .value_or(settings::VoiceLanguage::English);
            settings::SaveGame(value);
        } else {
            settings::SaveLanguageOverride(false);
        }
    }

    ImGui::BeginDisabled(!value.overrideLanguageSettings);
    constexpr const char* interfaceLanguages[]{
        "English",
        "German",
        "French",
        "Italian",
        "Korean",
        "Spanish",
        "Traditional Chinese",
        "Japanese"};
    constexpr settings::InterfaceLanguage interfaceValues[]{
        settings::InterfaceLanguage::English,
        settings::InterfaceLanguage::German,
        settings::InterfaceLanguage::French,
        settings::InterfaceLanguage::Italian,
        settings::InterfaceLanguage::Korean,
        settings::InterfaceLanguage::Spanish,
        settings::InterfaceLanguage::TraditionalChinese,
        settings::InterfaceLanguage::Japanese};
    widgets::SettingRow("Interface language", "Takes effect after restart");
    int interfaceLanguage = 0;
    for (int index = 0; index < static_cast<int>(std::size(interfaceValues));
         ++index) {
        if (interfaceValues[index] == value.interfaceLanguage) {
            interfaceLanguage = index;
            break;
        }
    }
    if (ImGui::Combo(
            "##InterfaceLanguage", &interfaceLanguage,
            interfaceLanguages,
            static_cast<int>(std::size(interfaceLanguages)))) {
        settings::SaveInterfaceLanguage(interfaceValues[interfaceLanguage]);
    }

    constexpr const char* voiceLanguages[]{"English", "Japanese"};
    widgets::SettingRow(
        "Voice language",
        "Takes effect after restart");
    int voiceLanguage =
        value.voiceLanguage == settings::VoiceLanguage::Japanese ? 1 : 0;
    if (title == game::Title::LightningReturns) {
        const auto current =
            game::language_override::CurrentVoiceLanguage(title);
        if (current) {
            voiceLanguage =
                *current == settings::VoiceLanguage::Japanese ? 1 : 0;
        }
    }
    if (ImGui::Combo(
            "##VoiceLanguage", &voiceLanguage,
            voiceLanguages,
            static_cast<int>(std::size(voiceLanguages)))) {
        const auto selected = voiceLanguage == 1
            ? settings::VoiceLanguage::Japanese
            : settings::VoiceLanguage::English;
        if (title == game::Title::LightningReturns) {
            game::language_override::ApplyVoiceLanguage(title, selected);
        } else {
            settings::SaveVoiceLanguage(selected);
        }
    }
    ImGui::EndDisabled();

    widgets::EndSettingsTable();
}

void DrawOverlaySettings() {
    if (!state::CollapsingHeader("Overlay", L"GeneralOverlay") ||
        !widgets::BeginSettingsTable("##OtherOverlaySettings")) {
        return;
    }

    settings::InputSettings input = settings::StoredInput();
    widgets::SettingRow("Center menu on open");
    if (ImGui::Checkbox(
            "##CenterOverlayOnOpen", &input.centerOverlayOnOpen)) {
        settings::SaveInput(input);
    }

    widgets::SettingRow("Remember menu state");
    if (ImGui::Checkbox(
            "##RememberOverlayState", &input.rememberOverlayState) &&
        settings::SaveInput(input)) {
        ImGui::GetIO().IniFilename = state::ImGuiIniPath();
    }

    widgets::EndSettingsTable();
}

void DrawSoundSettings() {
    if (!state::CollapsingHeader("Sounds", L"GeneralSounds") ||
        !widgets::BeginSettingsTable("##OtherSoundSettings")) {
        return;
    }

    settings::PerformanceSettings performance =
        settings::StoredPerformance();
    widgets::SettingRow(
        "Startup sound",
        "Plays once after NovaFix handles the first successful frame");
    if (ImGui::Checkbox("##StartupSound", &performance.startupSound)) {
        settings::SavePerformance(performance);
    }

    widgets::SettingRow(
        "Menu sounds",
        "Plays a subtle sound when the NovaFix menu opens or closes");
    if (ImGui::Checkbox("##MenuSounds", &performance.menuSounds)) {
        settings::SavePerformance(performance);
    }

    widgets::EndSettingsTable();
}

} // namespace

void DrawOtherPanel() {
    DrawGameLanguage();
    DrawOverlaySettings();
    DrawSoundSettings();

    const ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyCtrl || !io.KeyShift || !io.KeyAlt) return;

    constexpr const char* emptyMessage = "It's lonely here :(";
    const ImVec2 emptyStart = ImGui::GetCursorPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImGui::PushFont(
        nullptr, ImGui::GetStyle().FontSizeBase * (26.0f / 15.0f));
    const ImVec2 messageSize = ImGui::CalcTextSize(emptyMessage);
    ImGui::SetCursorPos(ImVec2(
        emptyStart.x + std::max(0.0f, (available.x - messageSize.x) * 0.5f),
        emptyStart.y + std::max(0.0f, (available.y - messageSize.y) * 0.5f)));
    ImGui::TextDisabled("%s", emptyMessage);
    ImGui::PopFont();
}

} // namespace novafix::ui
