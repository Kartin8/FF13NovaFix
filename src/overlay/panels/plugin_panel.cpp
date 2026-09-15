#include "overlay/panels/plugin_panel.h"

#include "compat/addons/addon_loader.h"
#include "compat/addons/plugin_controls.h"
#include "overlay/panels/settings_widgets.h"
#include "settings/performance_settings.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>

namespace novafix::ui {
namespace {

constexpr const char* kLoadedModulesNotificationLabels[]{
    "Never",
    "When add-ons list changes",
    "Every launch",
};

void DrawExtraSettings() {
    if (!widgets::BeginSettingsTable("##AddonNotificationSettings")) return;

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

    widgets::SettingRow("Add-on notification");
    int mode = static_cast<int>(
        performance.loadedModulesNotificationMode);
    if (ImGui::Combo(
            "##AddonNotification", &mode,
            kLoadedModulesNotificationLabels,
            static_cast<int>(std::size(kLoadedModulesNotificationLabels)))) {
        performance.loadedModulesNotificationMode =
            static_cast<settings::LoadedModulesNotificationMode>(mode);
        settings::SavePerformance(performance);
    }

    widgets::EndSettingsTable();
}

float Quantize(float value, float minimum, float maximum, float step) {
    const float steps = std::round((value - minimum) / step);
    return std::clamp(minimum + steps * step, minimum, maximum);
}

bool UsesSettingsRow(const compat::plugin_controls::Control& control) {
    return control.type == NOVAFIX_PLUGIN_CONTROL_CHECKBOX ||
           control.type == NOVAFIX_PLUGIN_CONTROL_INT_SLIDER ||
           control.type == NOVAFIX_PLUGIN_CONTROL_FLOAT_SLIDER ||
           control.type == NOVAFIX_PLUGIN_CONTROL_COMBO;
}

void DrawValueControl(const compat::plugin_controls::Plugin& plugin,
                      const compat::plugin_controls::Control& control) {
    widgets::SettingRow(control.label.c_str(),
                        control.description.empty() ? nullptr
                                                    : control.description.c_str());
    ImGui::PushID(control.id.c_str());

    if (!control.available) {
        ImGui::TextDisabled("%s", control.issue.c_str());
        ImGui::PopID();
        return;
    }

    float rawValue = 0.0f;
    if (!compat::plugin_controls::ReadValue(plugin, control, &rawValue)) {
        ImGui::TextDisabled("Could not read this add-on setting");
        ImGui::PopID();
        return;
    }

    if (control.type == NOVAFIX_PLUGIN_CONTROL_CHECKBOX) {
        bool value = rawValue >= 0.5f;
        if (ImGui::Checkbox("##Value", &value)) {
            compat::plugin_controls::WriteValue(
                plugin, control, value ? 1.0f : 0.0f);
        }
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_INT_SLIDER) {
        const int minimum = static_cast<int>(std::ceil(control.minimum));
        const int maximum = static_cast<int>(std::floor(control.maximum));
        int value = std::clamp(static_cast<int>(std::lround(rawValue)),
                               minimum, maximum);
        if (ImGui::SliderInt("##Value", &value, minimum, maximum)) {
            const float quantized = Quantize(
                static_cast<float>(value), control.minimum,
                control.maximum, control.step);
            compat::plugin_controls::WriteValue(
                plugin, control, std::round(quantized));
        }
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_FLOAT_SLIDER) {
        float value = std::clamp(rawValue, control.minimum, control.maximum);
        const char* format = control.step < 0.01f ? "%.3f" : "%.2f";
        if (ImGui::SliderFloat("##Value", &value, control.minimum,
                               control.maximum, format)) {
            compat::plugin_controls::WriteValue(
                plugin, control, Quantize(value, control.minimum,
                                          control.maximum, control.step));
        }
    } else if (!control.options.empty()) {
        const int maximum = static_cast<int>(control.options.size() - 1u);
        int value = std::clamp(static_cast<int>(std::lround(rawValue)),
                               0, maximum);
        if (ImGui::BeginCombo("##Value", control.options[value].c_str())) {
            for (int option = 0; option <= maximum; ++option) {
                const bool selected = option == value;
                if (ImGui::Selectable(control.options[option].c_str(),
                                      selected)) {
                    compat::plugin_controls::WriteValue(
                        plugin, control, static_cast<float>(option));
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::PopID();
}

void DrawStandaloneControl(const compat::plugin_controls::Plugin& plugin,
                           const compat::plugin_controls::Control& control) {
    ImGui::PushID(control.id.c_str());
    if (!control.available) {
        ImGui::TextDisabled("%s: %s", control.label.c_str(),
                            control.issue.c_str());
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_BUTTON) {
        if (control.callbackFailed) {
            ImGui::TextDisabled("%s: add-on callback failed",
                                control.label.c_str());
        } else if (ImGui::Button(control.label.c_str())) {
            compat::plugin_controls::ActivateControl(plugin, control);
        }
        widgets::HelpMarker(control.description.empty()
                                ? nullptr
                                : control.description.c_str());
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_HEADER) {
        ImGui::Spacing();
        ImGui::TextUnformatted(control.label.c_str());
        ImGui::Separator();
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_SEPARATOR) {
        ImGui::Separator();
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_TEXT) {
        ImGui::TextWrapped("%s", control.label.c_str());
    } else {
        ImGui::TextDisabled("%s: unsupported control",
                            control.label.c_str());
    }
    ImGui::PopID();
}

} // namespace

void DrawPluginPanel() {
    DrawExtraSettings();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        "Prefix add-on filenames with 00_, 01_, etc. to control load order");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const auto& plugins = compat::plugin_controls::CurrentPlugins();
    if (plugins.empty()) {
        if (compat::addon_loader::LoadedCount() == 0) {
            ImGui::TextDisabled(
                "Drop .dll or .asi files into the addons folder");
        } else {
            ImGui::TextDisabled(
                "Loaded add-ons have no settings");
        }
        return;
    }

    for (size_t pluginIndex = 0; pluginIndex < plugins.size(); ++pluginIndex) {
        const compat::plugin_controls::Plugin& plugin = plugins[pluginIndex];
        ImGui::PushID(static_cast<int>(pluginIndex));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * (17.0f / 15.0f));
        ImGui::TextUnformatted(plugin.name.c_str());
        ImGui::PopFont();
        ImGui::Separator();
        if (plugin.controls.empty()) {
            ImGui::TextDisabled("This add-on has no settings");
        } else {
            bool tableOpen = false;
            std::size_t tableIndex = 0u;
            for (size_t controlIndex = 0; controlIndex < plugin.controls.size();
                 ++controlIndex) {
                const compat::plugin_controls::Control& control =
                    plugin.controls[controlIndex];
                if (UsesSettingsRow(control)) {
                    if (!tableOpen) {
                        char tableId[48]{};
                        std::snprintf(tableId, sizeof(tableId),
                                      "##PluginControls%zu", tableIndex++);
                        tableOpen = widgets::BeginSettingsTable(tableId);
                    }
                    if (tableOpen) {
                        ImGui::PushID(static_cast<int>(controlIndex));
                        DrawValueControl(plugin, control);
                        ImGui::PopID();
                    }
                } else {
                    if (tableOpen) {
                        widgets::EndSettingsTable();
                        tableOpen = false;
                    }
                    ImGui::PushID(static_cast<int>(controlIndex));
                    DrawStandaloneControl(plugin, control);
                    ImGui::PopID();
                }
            }
            if (tableOpen) widgets::EndSettingsTable();
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
}

} // namespace novafix::ui
