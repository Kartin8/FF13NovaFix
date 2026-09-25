#include "overlay/panels/plugin_panel.h"

#include "compat/addons/addon_loader.h"
#include "compat/addons/plugin_controls.h"
#include "overlay/overlay_style.h"
#include "overlay/panels/settings_widgets.h"
#include "overlay/ui_state.h"
#include "settings/performance_settings.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>

namespace novafix::ui {
namespace {

constexpr const char* kLoadedModulesNotificationLabels[]{
    "Never",
    "When add-ons list changes",
    "Every launch",
};

std::wstring PluginOpenStateKey(std::string_view pluginId) {
    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffset;
    for (unsigned char character : pluginId) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<unsigned char>(character + ('a' - 'A'));
        }
        hash ^= character;
        hash *= kPrime;
    }
    return L"AddonHeaderOpen_" + std::to_wstring(hash);
}

std::string PresentationNote(
    const compat::plugin_controls::Control& control) {
    std::string note;
    const auto append = [&](const char* value) {
        if (!note.empty()) note.append(" · ");
        note.append(value);
    };
    if (control.presentationFlags & NOVAFIX_PLUGIN_PRESENTATION_ADVANCED) {
        append("Advanced");
    }
    if (control.presentationFlags &
        NOVAFIX_PLUGIN_PRESENTATION_RESTART_REQUIRED) {
        append("Restart");
    }
    if (control.presentationFlags &
        NOVAFIX_PLUGIN_PRESENTATION_EXPERIMENTAL) {
        append("Experimental");
    }
    return note;
}

std::string PluginMetadata(
    const compat::plugin_controls::Plugin& plugin) {
    if (!plugin.version.empty() && !plugin.author.empty()) {
        return "v" + plugin.version + " by " + plugin.author;
    }
    if (!plugin.version.empty()) return "v" + plugin.version;
    if (!plugin.author.empty()) return "by " + plugin.author;
    return {};
}

ImVec4 ResolveHeaderColor(const NovaFixPluginColor& requested,
                          const ImVec4& fallback, float lift,
                          float peakLimit) {
    ImVec4 custom{
        std::clamp(requested.red, 0.0f, 1.0f),
        std::clamp(requested.green, 0.0f, 1.0f),
        std::clamp(requested.blue, 0.0f, 1.0f), 1.0f};
    const float customPeak = std::max({custom.x, custom.y, custom.z});
    if (customPeak > 0.44f) {
        const float scale = 0.44f / customPeak;
        custom.x *= scale;
        custom.y *= scale;
        custom.z *= scale;
    }

    const float influence = std::clamp(requested.alpha, 0.0f, 1.0f);
    ImVec4 result{
        fallback.x + (custom.x - fallback.x) * influence,
        fallback.y + (custom.y - fallback.y) * influence,
        fallback.z + (custom.z - fallback.z) * influence,
        1.0f};
    result.x += (1.0f - result.x) * lift;
    result.y += (1.0f - result.y) * lift;
    result.z += (1.0f - result.z) * lift;
    const float peak = std::max({result.x, result.y, result.z});
    if (peak > peakLimit) {
        const float scale = peakLimit / peak;
        result.x *= scale;
        result.y *= scale;
        result.z *= scale;
    }
    return result;
}

ImVec4 ResolveIndicatorColor(const NovaFixPluginColor& requested,
                             const ImVec4& fallback) {
    const float red = std::clamp(requested.red, 0.0f, 1.0f);
    const float green = std::clamp(requested.green, 0.0f, 1.0f);
    const float blue = std::clamp(requested.blue, 0.0f, 1.0f);
    const float peak = std::max({red, green, blue});
    if (peak <= 0.001f) return fallback;

    const ImVec4 lightAccent{
        0.58f + (red / peak) * 0.38f,
        0.58f + (green / peak) * 0.38f,
        0.58f + (blue / peak) * 0.38f,
        1.0f};
    const float influence = std::clamp(requested.alpha, 0.0f, 1.0f);
    return ImVec4(
        fallback.x + (lightAccent.x - fallback.x) * influence,
        fallback.y + (lightAccent.y - fallback.y) * influence,
        fallback.z + (lightAccent.z - fallback.z) * influence,
        1.0f);
}

class PluginThemeScope {
public:
    explicit PluginThemeScope(
        const compat::plugin_controls::Plugin& plugin) {
        if (!plugin.hasHeaderColor || !plugin.headerColorAppliesToContent) {
            return;
        }
        const NovaFixPluginColor& color = plugin.headerColor;
        push(ImGuiCol_Header, color, 0.00f, 0.46f);
        push(ImGuiCol_HeaderHovered, color, 0.06f, 0.54f);
        push(ImGuiCol_HeaderActive, color, 0.10f, 0.60f);
        push(ImGuiCol_FrameBg, color, 0.00f, 0.34f);
        push(ImGuiCol_FrameBgHovered, color, 0.05f, 0.44f);
        push(ImGuiCol_FrameBgActive, color, 0.09f, 0.52f);
        push(ImGuiCol_Button, color, 0.02f, 0.42f);
        push(ImGuiCol_ButtonHovered, color, 0.08f, 0.53f);
        push(ImGuiCol_ButtonActive, color, 0.12f, 0.59f);
        ImGui::PushStyleColor(
            ImGuiCol_CheckMark,
            ResolveIndicatorColor(
                color, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark)));
        ++colorCount_;
        push(ImGuiCol_SliderGrab, color, 0.16f, 0.70f);
        push(ImGuiCol_SliderGrabActive, color, 0.24f, 0.82f);
    }

    ~PluginThemeScope() {
        if (colorCount_) ImGui::PopStyleColor(colorCount_);
    }

    PluginThemeScope(const PluginThemeScope&) = delete;
    PluginThemeScope& operator=(const PluginThemeScope&) = delete;

private:
    void push(ImGuiCol target, const NovaFixPluginColor& color,
              float lift, float peakLimit) {
        ImGui::PushStyleColor(
            target, ResolveHeaderColor(
                        color, ImGui::GetStyleColorVec4(target), lift,
                        peakLimit));
        ++colorCount_;
    }

    int colorCount_{};
};

void DrawExtraSettings() {
    if (!widgets::BeginSettingsTable("##AddonNotificationSettings")) return;

    settings::PerformanceSettings performance =
        settings::StoredPerformance();
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
           control.type == NOVAFIX_PLUGIN_CONTROL_COMBO ||
           control.type == NOVAFIX_PLUGIN_CONTROL_RADIO_GROUP ||
           control.type == NOVAFIX_PLUGIN_CONTROL_STATUS_TEXT ||
           control.type == NOVAFIX_PLUGIN_CONTROL_COLOR;
}

void DrawValueControl(const compat::plugin_controls::Plugin& plugin,
                      const compat::plugin_controls::Control& control,
                      std::uint32_t state) {
    const std::string note = PresentationNote(control);
    widgets::SettingRow(control.label.c_str(),
                        control.description.empty() ? nullptr
                                                    : control.description.c_str(),
                        note.empty() ? nullptr : note.c_str());
    ImGui::PushID(control.id.c_str());

    if (!control.available) {
        ImGui::TextDisabled("%s", control.issue.c_str());
        ImGui::PopID();
        return;
    }

    const bool enabled =
        (state & NOVAFIX_PLUGIN_CONTROL_STATE_ENABLED) != 0u &&
        (state & (NOVAFIX_PLUGIN_CONTROL_STATE_READ_ONLY |
                  NOVAFIX_PLUGIN_CONTROL_STATE_BUSY)) == 0u;

    if (control.type == NOVAFIX_PLUGIN_CONTROL_STATUS_TEXT) {
        const char* value = nullptr;
        if (compat::plugin_controls::ReadText(plugin, control, &value)) {
            ImGui::TextWrapped("%s", value);
        } else {
            ImGui::TextDisabled("Could not read status");
        }
        ImGui::PopID();
        return;
    }

    if (control.type == NOVAFIX_PLUGIN_CONTROL_COLOR) {
        NovaFixPluginColor value{};
        if (!compat::plugin_controls::ReadColor(plugin, control, &value)) {
            ImGui::TextDisabled("Could not read this color");
            ImGui::PopID();
            return;
        }
        float components[]{value.red, value.green, value.blue, value.alpha};
        ImGui::BeginDisabled(!enabled);
        const bool changed =
            (control.colorFlags & NOVAFIX_PLUGIN_COLOR_ALPHA) != 0u
                ? ImGui::ColorEdit4("##Value", components,
                                    ImGuiColorEditFlags_AlphaBar)
                : ImGui::ColorEdit3("##Value", components);
        ImGui::EndDisabled();
        if (changed) {
            value = {components[0], components[1], components[2],
                     components[3]};
            compat::plugin_controls::WriteColor(plugin, control, value);
        }
        ImGui::PopID();
        return;
    }

    float rawValue = 0.0f;
    if (!compat::plugin_controls::ReadValue(plugin, control, &rawValue)) {
        ImGui::TextDisabled("Could not read this add-on setting");
        ImGui::PopID();
        return;
    }

    ImGui::BeginDisabled(!enabled);
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
        const char* format = control.valueFormat.empty()
                                 ? "%d" : control.valueFormat.c_str();
        if (ImGui::SliderInt(
                "##Value", &value, minimum, maximum, format)) {
            const float quantized = Quantize(
                static_cast<float>(value), control.minimum,
                control.maximum, control.step);
            compat::plugin_controls::WriteValue(
                plugin, control, std::round(quantized));
        }
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_FLOAT_SLIDER) {
        float value = std::clamp(rawValue, control.minimum, control.maximum);
        const char* format = control.valueFormat.empty()
                                 ? (control.step < 0.01f ? "%.3f" : "%.2f")
                                 : control.valueFormat.c_str();
        if (ImGui::SliderFloat("##Value", &value, control.minimum,
                               control.maximum, format)) {
            compat::plugin_controls::WriteValue(
                plugin, control, Quantize(value, control.minimum,
                                          control.maximum, control.step));
        }
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_RADIO_GROUP &&
               !control.options.empty()) {
        const int maximum = static_cast<int>(control.options.size() - 1u);
        const int value = std::clamp(
            static_cast<int>(std::lround(rawValue)), 0, maximum);
        const float available = ImGui::GetContentRegionAvail().x;
        const ImGuiStyle& style = ImGui::GetStyle();
        float used = 0.0f;
        for (int option = 0; option <= maximum; ++option) {
            const char* const text = control.options[option].c_str();
            const float width = ImGui::GetFrameHeight() +
                style.ItemInnerSpacing.x + ImGui::CalcTextSize(text).x;
            const float spacing = option == 0 ? 0.0f : style.ItemSpacing.x;
            if (option != 0 && used + spacing + width <= available) {
                ImGui::SameLine(0.0f, spacing);
                used += spacing;
            } else if (option != 0) {
                used = 0.0f;
            }
            ImGui::PushID(option);
            if (ImGui::RadioButton(text, option == value)) {
                compat::plugin_controls::WriteValue(
                    plugin, control, static_cast<float>(option));
            }
            ImGui::PopID();
            used += width;
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
    ImGui::EndDisabled();

    ImGui::PopID();
}

void DrawStandaloneControl(const compat::plugin_controls::Plugin& plugin,
                           const compat::plugin_controls::Control& control,
                           std::uint32_t state) {
    ImGui::PushID(control.id.c_str());
    if (!control.available) {
        ImGui::TextDisabled("%s: %s", control.label.c_str(),
                            control.issue.c_str());
    } else if (control.type == NOVAFIX_PLUGIN_CONTROL_BUTTON) {
        if (control.callbackFailed) {
            ImGui::TextDisabled("%s: add-on callback failed",
                                control.label.c_str());
        } else {
            const bool enabled =
                (state & NOVAFIX_PLUGIN_CONTROL_STATE_ENABLED) != 0u &&
                (state & (NOVAFIX_PLUGIN_CONTROL_STATE_READ_ONLY |
                          NOVAFIX_PLUGIN_CONTROL_STATE_BUSY)) == 0u;
            ImGui::BeginDisabled(!enabled);
            if (ImGui::Button(control.label.c_str())) {
                compat::plugin_controls::ActivateControl(plugin, control);
            }
            ImGui::EndDisabled();
            if ((state & NOVAFIX_PLUGIN_CONTROL_STATE_BUSY) != 0u) {
                ImGui::SameLine();
                ImGui::TextDisabled("Working...");
            }
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
    const std::string note = PresentationNote(control);
    if (!note.empty()) {
        ImGui::SameLine(0.0f, 7.0f);
        ImGui::TextDisabled("%s", note.c_str());
    }
    ImGui::PopID();
}

} // namespace

void DrawPluginPanel() {
    DrawExtraSettings();
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

    for (std::size_t pluginIndex = 0u; pluginIndex < plugins.size();
         ++pluginIndex) {
        const compat::plugin_controls::Plugin& plugin = plugins[pluginIndex];
        ImGui::PushID(static_cast<int>(pluginIndex));
        const std::string metadata = PluginMetadata(plugin);
        const std::string headerLabel =
            plugin.name + "###PluginHeader";
        ImFont* titleFont =
            overlay::style::PluginTitleFont(plugin.name.c_str());
        ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 headerPadding(
            style.FramePadding.x,
            style.FramePadding.y + style.ItemInnerSpacing.y * 0.5f);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, headerPadding);
        if (plugin.hasHeaderColor) {
            ImGui::PushStyleColor(
                ImGuiCol_Header,
                ResolveHeaderColor(
                    plugin.headerColor,
                    ImVec4(0.075f, 0.16f, 0.27f, 1.0f), 0.0f, 0.48f));
            ImGui::PushStyleColor(
                ImGuiCol_HeaderHovered,
                ResolveHeaderColor(
                    plugin.headerColor,
                    ImVec4(0.10f, 0.25f, 0.42f, 1.0f), 0.06f, 0.56f));
            ImGui::PushStyleColor(
                ImGuiCol_HeaderActive,
                ResolveHeaderColor(
                    plugin.headerColor,
                    ImVec4(0.12f, 0.29f, 0.48f, 1.0f), 0.10f, 0.62f));
        } else {
            ImGui::PushStyleColor(
                ImGuiCol_Header, ImVec4(0.075f, 0.16f, 0.27f, 0.96f));
            ImGui::PushStyleColor(
                ImGuiCol_HeaderHovered, ImVec4(0.10f, 0.25f, 0.42f, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_HeaderActive, ImVec4(0.12f, 0.29f, 0.48f, 1.0f));
        }
        ImGui::PushFont(
            titleFont,
            titleFont ? titleFont->LegacySize
                      : ImGui::GetStyle().FontSizeBase * (16.5f / 15.0f));
        const float titleWidth = ImGui::CalcTextSize(plugin.name.c_str()).x;
        const float titleStartX =
            ImGui::GetCursorScreenPos().x + ImGui::GetFontSize() +
            headerPadding.x * 3.0f;
        const std::wstring headerStateKey = PluginOpenStateKey(plugin.id);
        const bool pluginOpen = state::CollapsingHeader(
            headerLabel.c_str(), headerStateKey.c_str());
        const ImVec2 headerMin = ImGui::GetItemRectMin();
        const ImVec2 headerMax = ImGui::GetItemRectMax();
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        if (!metadata.empty()) {
            const ImVec2 metadataSize =
                ImGui::CalcTextSize(metadata.c_str());
            const float titleEndX = titleStartX + titleWidth;
            const float metadataX =
                headerMax.x - headerPadding.x - metadataSize.x;
            if (metadataX > titleEndX + style.ItemSpacing.x * 1.5f) {
                const float metadataY =
                    headerMin.y +
                    (headerMax.y - headerMin.y - metadataSize.y) * 0.5f;
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(metadataX, metadataY),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    metadata.c_str());
            }
        }

        if (!pluginOpen) {
            ImGui::Spacing();
            ImGui::PopID();
            continue;
        }

        const PluginThemeScope pluginTheme(plugin);
        ImGui::Spacing();
        if (!plugin.description.empty()) {
            ImGui::TextWrapped("%s", plugin.description.c_str());
        }
        if (!plugin.website.empty()) {
            ImGui::TextLinkOpenURL("Website", plugin.website.c_str());
        }
        if (!plugin.description.empty() || !plugin.website.empty()) {
            ImGui::Spacing();
        }

        if (plugin.controls.empty()) {
            ImGui::TextDisabled("This add-on has no settings");
            ImGui::PopID();
            continue;
        }

        bool tableOpen = false;
        bool sectionOpen = true;
        std::size_t tableIndex = 0u;
        for (std::size_t controlIndex = 0u;
             controlIndex < plugin.controls.size(); ++controlIndex) {
            const compat::plugin_controls::Control& control =
                plugin.controls[controlIndex];
            const std::uint32_t state =
                compat::plugin_controls::ReadControlState(plugin, control);
            if (control.type == NOVAFIX_PLUGIN_CONTROL_SECTION) {
                if (tableOpen) {
                    widgets::EndSettingsTable();
                    tableOpen = false;
                }
                if ((state & NOVAFIX_PLUGIN_CONTROL_STATE_VISIBLE) == 0u) {
                    sectionOpen = false;
                    continue;
                }
                ImGui::PushID(static_cast<int>(controlIndex));
                ImGuiTreeNodeFlags flags =
                    ImGuiTreeNodeFlags_SpanAvailWidth;
                if (!(control.presentationFlags &
                      NOVAFIX_PLUGIN_PRESENTATION_COLLAPSED_BY_DEFAULT)) {
                    flags |= ImGuiTreeNodeFlags_DefaultOpen;
                }
                const std::wstring sectionStateKey =
                    PluginOpenStateKey(plugin.id) + L"_Section_" +
                    std::to_wstring(controlIndex);
                sectionOpen = state::CollapsingHeader(
                    control.label.c_str(), sectionStateKey.c_str(), flags);
                if (!control.description.empty() &&
                    ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                    ImGui::TextUnformatted(control.description.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
                continue;
            }
            if (!sectionOpen ||
                (state & NOVAFIX_PLUGIN_CONTROL_STATE_VISIBLE) == 0u) {
                continue;
            }
            if (UsesSettingsRow(control)) {
                if (!tableOpen) {
                    char tableId[48]{};
                    std::snprintf(tableId, sizeof(tableId),
                                  "##PluginControls%zu", tableIndex++);
                    tableOpen = widgets::BeginSettingsTable(tableId);
                }
                if (tableOpen) {
                    ImGui::PushID(static_cast<int>(controlIndex));
                    DrawValueControl(plugin, control, state);
                    ImGui::PopID();
                }
            } else {
                if (tableOpen) {
                    widgets::EndSettingsTable();
                    tableOpen = false;
                }
                ImGui::PushID(static_cast<int>(controlIndex));
                DrawStandaloneControl(plugin, control, state);
                ImGui::PopID();
            }
        }
        if (tableOpen) widgets::EndSettingsTable();
        ImGui::Spacing();
        ImGui::PopID();
    }
}

} // namespace novafix::ui
