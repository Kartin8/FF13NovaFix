#include "overlay/panels/settings_widgets.h"

#include "imgui.h"

#include <algorithm>

namespace novafix::ui::widgets {

void HelpMarker(const char* text) {
    if (!text || !*text) return;
    ImGui::SameLine(0.0f, 5.0f);
    ImGui::TextDisabled("?");
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

bool BeginSettingsTable(const char* identifier, float labelWidth) {
    if (!ImGui::BeginTable(identifier, 2, ImGuiTableFlags_SizingStretchProp |
                                           ImGuiTableFlags_PadOuterX)) {
        return false;
    }
    const float scale = std::max(1.0f, ImGui::GetFontSize() / 15.0f);
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed,
                            labelWidth * scale);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void EndSettingsTable() {
    ImGui::EndTable();
}

void SettingRow(const char* label, const char* help) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    HelpMarker(help);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
}

} // namespace novafix::ui::widgets
