#include "overlay/panels/compatibility_panel.h"

#include "compat/compatibility.h"

#include "imgui.h"

namespace novafix::ui {
void DrawCompatibilityBanner() {
    const compat::compatibility::Report report = compat::compatibility::CurrentReport();
    if (report.blocked.empty() && report.active.empty()) return;

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.28f, 1.0f));
    if (!report.active.empty()) {
        ImGui::TextWrapped(
            "FF13Fix or FF13Fix PLUS is already running. "
            "Conflicting fixes were disabled");
    } else {
        ImGui::TextWrapped(
            "FF13Fix or FF13Fix PLUS was blocked before conflicting fixes could load");
    }
    ImGui::PopStyleColor();
    ImGui::Separator();
}

} // namespace novafix::ui
