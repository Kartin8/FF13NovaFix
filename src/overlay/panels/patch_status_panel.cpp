#include "overlay/panels/patch_status_panel.h"

#include "patch/registry.h"

#include "imgui.h"

namespace novafix::ui {
void DrawPatchSafetyBanner() {
    const patch::registry::Summary summary = patch::registry::CurrentSummary();
    if (!summary.conflicts && !summary.integrityLost) return;

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.48f, 0.30f, 1.0f));
    if (summary.integrityLost) {
        ImGui::TextWrapped(
            "Another mod changed this fix after it was installed. No further changes were made");
    } else {
        ImGui::TextWrapped("Another mod already changed the same part of the game. No changes were made");
    }
    ImGui::PopStyleColor();
    ImGui::Separator();
}

} // namespace novafix::ui
