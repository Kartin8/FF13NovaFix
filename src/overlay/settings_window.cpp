#include "overlay/settings_window.h"

#include "game/core/game_profile.h"
#include "overlay/panels/compatibility_panel.h"
#include "overlay/panels/diagnostics_panel.h"
#include "overlay/panels/graphics_panel.h"
#include "overlay/panels/input_panel.h"
#include "overlay/panels/patch_status_panel.h"
#include "overlay/panels/performance_panel.h"
#include "overlay/panels/plugin_panel.h"
#include "runtime/restart.h"

#include "imgui.h"

#include <algorithm>

namespace novafix::ui {
namespace {

bool g_restarting = false;
ULONGLONG g_restartFailedUntil{};
void (*g_currentPanel)(){};

void DrawTab(const char* name, void (*draw)()) {
    if (!ImGui::BeginTabItem(name)) return;
    if (g_currentPanel != draw) {
        FinishSettingsEdits();
        g_currentPanel = draw;
    }
    draw();
    ImGui::EndTabItem();
}

void DrawTabs() {
    const bool showGraphics = game::CurrentProfile().title !=
                              game::Title::LightningReturns;

    if (!ImGui::BeginTabBar("##NovaFixTabs")) return;
    if (showGraphics) DrawTab("Graphics", &DrawGraphicsPanel);
    DrawTab("Performance", &DrawPerformancePanel);
    DrawTab("Input", &DrawInputPanel);
    DrawTab("Diagnostics", &DrawDiagnosticsPanel);
    DrawTab("Add-ons & extra", &DrawPluginPanel);
    ImGui::EndTabBar();
}

void DrawRestart(HWND gameWindow) {
    ImGui::Separator();
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 15.0f);
    const float width = 100.0f * uiScale;
    const float rowStart = ImGui::GetCursorPosX();
    const float rightEdge = rowStart + ImGui::GetContentRegionAvail().x;

    if (GetTickCount64() < g_restartFailedUntil) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.42f, 0.32f, 1.0f), "Restart failed");
        ImGui::SameLine();
    }

    ImGui::SetCursorPosX(std::max(rowStart, rightEdge - width));
    ImGui::BeginDisabled(g_restarting);
    if (ImGui::Button("Restart", ImVec2(width, 0.0f))) {
        FinishSettingsEdits();
        g_restarting = restart::ScheduleCurrentProcessRestart(gameWindow);
        if (!g_restarting) g_restartFailedUntil = GetTickCount64() + 3500;
    }
    ImGui::EndDisabled();
}

} // namespace

void FinishSettingsEdits() {
    FinishGraphicsEdit();
    FinishPerformanceEdit();
    FinishInputEdit();
    g_currentPanel = nullptr;
}

void DrawSettingsWindow(HWND gameWindow) {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 15.0f);
    ImGui::BeginChild(
        "##NovaFixContent", ImVec2(0.0f, -38.0f * uiScale),
        ImGuiChildFlags_AlwaysUseWindowPadding |
            ImGuiChildFlags_NavFlattened);
    DrawCompatibilityBanner();
    DrawPatchSafetyBanner();
    DrawTabs();
    ImGui::EndChild();
    DrawRestart(gameWindow);
}

} // namespace novafix::ui
