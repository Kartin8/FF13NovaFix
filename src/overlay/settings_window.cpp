#include "overlay/settings_window.h"

#include "game/core/game_profile.h"
#include "overlay/panels/compatibility_panel.h"
#include "overlay/panels/diagnostics_panel.h"
#include "overlay/panels/graphics_panel.h"
#include "overlay/panels/input_panel.h"
#include "overlay/panels/other_panel.h"
#include "overlay/panels/patch_status_panel.h"
#include "overlay/panels/performance_panel.h"
#include "overlay/panels/plugin_panel.h"
#include "overlay/ui_state.h"
#include "runtime/restart.h"

#include "imgui.h"

#include <algorithm>
#include <limits>

namespace novafix::ui {
namespace {

bool g_restarting = false;
ULONGLONG g_restartFailedUntil{};
void (*g_currentPanel)(){};
unsigned g_selectedTab{};
unsigned g_visibleTab{};
bool g_restoreTabPending{};
bool g_tabClickedThisFrame{};

void DrawTab(const char* name, unsigned index, void (*draw)()) {
    const ImGuiTabItemFlags flags =
        g_restoreTabPending && index == g_selectedTab
            ? ImGuiTabItemFlags_SetSelected : 0;
    const bool visible = ImGui::BeginTabItem(name, nullptr, flags);
    if (ImGui::IsItemClicked() && index != g_selectedTab) {
        g_selectedTab = index;
        g_tabClickedThisFrame = true;
        state::SaveSelectedTab(index);
    }
    if (!visible) return;
    g_visibleTab = index;
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
    if (g_selectedTab > 5u || (!showGraphics && g_selectedTab == 1u)) {
        g_selectedTab = 0u;
    }

    if (!ImGui::BeginTabBar("##NovaFixTabs")) return;
    g_visibleTab = std::numeric_limits<unsigned>::max();
    g_tabClickedThisFrame = false;
    DrawTab("General", 0u, &DrawOtherPanel);
    if (showGraphics) DrawTab("Graphics", 1u, &DrawGraphicsPanel);
    DrawTab("Performance", 2u, &DrawPerformancePanel);
    DrawTab("Input", 3u, &DrawInputPanel);
    DrawTab("Diagnostics", 4u, &DrawDiagnosticsPanel);
    DrawTab("Add-ons", 5u, &DrawPluginPanel);
    ImGui::EndTabBar();
    if (g_restoreTabPending) {
        g_restoreTabPending = false;
    } else if (!g_tabClickedThisFrame &&
               g_visibleTab != std::numeric_limits<unsigned>::max() &&
               g_visibleTab != g_selectedTab) {
        g_selectedTab = g_visibleTab;
        state::SaveSelectedTab(g_selectedTab);
    }
}

void DrawRestart(HWND gameWindow) {
    ImGui::Separator();
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 15.0f);
    const float width = 100.0f * uiScale;
    const float rowStart = ImGui::GetCursorPosX();
    const float rightEdge = rowStart + ImGui::GetContentRegionAvail().x;

    bool leftContent = false;
    if (GetTickCount64() < g_restartFailedUntil) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.42f, 0.32f, 1.0f), "Restart failed");
        leftContent = true;
    } else if (g_currentPanel == &DrawPluginPanel) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(
            "Prefix filenames with 00_, 01_, etc. to control add-on load order");
        leftContent = true;
    }

    if (leftContent) ImGui::SameLine();
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

void RestoreSettingsWindowState() {
    g_selectedTab = state::SelectedTab();
    g_restoreTabPending = true;
    g_currentPanel = nullptr;
}

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
            ImGuiChildFlags_NavFlattened,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    DrawCompatibilityBanner();
    DrawPatchSafetyBanner();
    DrawTabs();
    ImGui::EndChild();
    DrawRestart(gameWindow);
}

} // namespace novafix::ui
