#include "overlay/panels/input_panel.h"

#include "game/core/game_profile.h"
#include "overlay/input/overlay_hotkey.h"
#include "settings/input_settings.h"
#include "overlay/panels/settings_widgets.h"
#include "settings/performance_settings.h"

#include "imgui.h"

#include <iterator>

namespace novafix::ui {
namespace {

settings::InputSettings g_settings{};
bool g_ready = false;
bool g_editPending = false;

constexpr const char* kPromptModes[]{
    "Auto",
    "Controller",
    "Keyboard",
};

constexpr settings::OverlayHotkey kOverlayHotkeys[]{
    settings::OverlayHotkey::F8,
    settings::OverlayHotkey::F9,
    settings::OverlayHotkey::F10,
    settings::OverlayHotkey::F11,
    settings::OverlayHotkey::F12,
    settings::OverlayHotkey::Grave,
    settings::OverlayHotkey::Insert,
    settings::OverlayHotkey::Home,
};

constexpr settings::OverlayGamepadHotkey kOverlayGamepadHotkeys[]{
    settings::OverlayGamepadHotkey::Disabled,
    settings::OverlayGamepadHotkey::StickClicks,
    settings::OverlayGamepadHotkey::ViewMenu,
};

void EnsureSettings() {
    if (g_ready) return;
    g_settings = settings::StoredInput();
    g_ready = true;
}

void SaveSettings() {
    g_editPending = false;
    if (settings::SaveInput(g_settings)) return;
    g_settings = settings::StoredInput();
    settings::PreviewInput(g_settings);
}

bool DrawOverlayHotkey(const char* id, settings::OverlayHotkey& value) {
    bool changed = false;
    if (ImGui::BeginCombo(id, input::OverlayHotkeyName(value))) {
        for (const settings::OverlayHotkey candidate : kOverlayHotkeys) {
            const bool selected = candidate == value;
            if (ImGui::Selectable(input::OverlayHotkeyName(candidate), selected)) {
                value = candidate;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool DrawOverlayGamepadHotkey(const char* id,
                              settings::OverlayGamepadHotkey& value) {
    bool changed = false;
    if (ImGui::BeginCombo(id, input::OverlayGamepadHotkeyName(value))) {
        for (const settings::OverlayGamepadHotkey candidate :
             kOverlayGamepadHotkeys) {
            const bool selected = candidate == value;
            if (ImGui::Selectable(
                    input::OverlayGamepadHotkeyName(candidate), selected)) {
                value = candidate;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void DrawFastControllerReconnectSetting() {
    if (!game::Supports(game::Capability::ControllerHotplug)) return;
    settings::PerformanceSettings performance =
        settings::StoredPerformance();
    widgets::SettingRow(
        "Fast controller reconnect",
        "Updates the controller list when devices connect or disconnect instead of scanning continuously");
    if (ImGui::Checkbox(
            "##SmartControllerHotplug",
            &performance.smartControllerHotplug)) {
        settings::SavePerformance(performance);
    }
}

} // namespace

void FinishInputEdit() {
    if (g_editPending) SaveSettings();
}

void DrawInputPanel() {
    EnsureSettings();
    const bool promptsAvailable = game::Supports(game::Capability::DynamicPrompts);
    const bool vibrationAvailable = game::Supports(game::Capability::ControllerVibration);
    if (widgets::BeginSettingsTable("##InputSettings")) {
        widgets::SettingRow("Menu hotkey");
        if (DrawOverlayHotkey("##OverlayHotkey", g_settings.overlayHotkey)) {
            SaveSettings();
        }

        widgets::SettingRow("Controller shortcut");
        if (DrawOverlayGamepadHotkey(
                "##OverlayGamepadHotkey", g_settings.overlayGamepadHotkey)) {
            SaveSettings();
        }

        if (vibrationAvailable) {
            widgets::SettingRow("Vibration strength");
            ImGui::BeginDisabled(!g_settings.controllerVibration);
            int strength = static_cast<int>(
                g_settings.vibrationStrengthPercent);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##VibrationStrength", &strength, 10, 200, "%d%%")) {
                g_settings.vibrationStrengthPercent =
                    static_cast<unsigned>(strength);
                g_editPending = true;
                settings::PreviewInput(g_settings);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) FinishInputEdit();
            ImGui::EndDisabled();
        }

        if (promptsAvailable) {
            widgets::SettingRow("Button prompts");
            int mode = static_cast<int>(g_settings.promptMode);
            if (ImGui::Combo("##ButtonPrompts", &mode, kPromptModes,
                             static_cast<int>(std::size(kPromptModes)))) {
                g_settings.promptMode = static_cast<settings::PromptMode>(mode);
                SaveSettings();
            }
        }

        DrawFastControllerReconnectSetting();

        if (vibrationAvailable) {
            widgets::SettingRow("Controller vibration");
            if (ImGui::Checkbox(
                    "##ControllerVibration",
                    &g_settings.controllerVibration)) {
                SaveSettings();
            }
        }

        widgets::EndSettingsTable();
    }
}

} // namespace novafix::ui
