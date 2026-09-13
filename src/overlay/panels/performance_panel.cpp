#include "overlay/panels/performance_panel.h"

#include "game/core/game_profile.h"
#include "settings/graphics_settings.h"
#include "settings/performance_settings.h"
#include "overlay/panels/settings_widgets.h"

#include "imgui.h"

#include <algorithm>
#include <iterator>

namespace novafix::ui {
namespace {

struct FrameRatePreset {
    unsigned rate;
    const char* label;
};

constexpr FrameRatePreset kFrameRates[]{
    {30, "30 FPS"},
    {60, "60 FPS"},
    {90, "90 FPS"},
    {120, "120 FPS"},
    {144, "144 FPS"},
    {0, "Unlimited"},
};
constexpr const char* kVSyncLabels[]{"Game default", "Off", "On"};
constexpr const char* kTripleBufferingLabels[]{"Game default", "Off", "On"};
constexpr const char* kD3D9BackendLabels[]{
    "Automatic",
    "System Direct3D 9",
    "Local DXVK",
};
settings::PerformanceSettings g_settings{};
bool g_ready = false;
bool g_customRate = false;
bool g_editPending = false;

bool IsLightningReturns() {
    return game::CurrentProfile().title == game::Title::LightningReturns;
}

void PreserveOtherSettings(
    settings::PerformanceSettings& value) {
    const settings::PerformanceSettings stored = settings::StoredPerformance();
    value.smartControllerHotplug = stored.smartControllerHotplug;
    value.showFpsCounter = stored.showFpsCounter;
    value.loadedModulesNotificationMode = stored.loadedModulesNotificationMode;
    if (!IsLightningReturns()) return;
    value.frameRateLimit = stored.frameRateLimit;
    value.vsyncMode = stored.vsyncMode;
    value.refreshRate = stored.refreshRate;
    value.tripleBuffering = stored.tripleBuffering;
}

int FrameRateIndex(unsigned rate) {
    for (int i = 0; i < static_cast<int>(std::size(kFrameRates)); ++i) {
        if (kFrameRates[i].rate == rate) return i;
    }
    return -1;
}

void EnsureSettings() {
    if (g_ready) return;
    g_settings = settings::StoredPerformance();
    g_customRate = FrameRateIndex(g_settings.frameRateLimit) < 0;
    g_ready = true;
}

void PreviewSettings() {
    settings::PerformanceSettings preview = g_settings;
    PreserveOtherSettings(preview);
    settings::PreviewPerformance(preview);
}

void SaveSettings() {
    g_editPending = false;
    settings::PerformanceSettings value = g_settings;
    PreserveOtherSettings(value);
    if (settings::SavePerformance(value)) {
        g_settings = value;
        return;
    }
    g_settings = settings::StoredPerformance();
    PreviewSettings();
    g_customRate = FrameRateIndex(g_settings.frameRateLimit) < 0;
}

void DrawFrameRate() {
    int selected = FrameRateIndex(g_settings.frameRateLimit);
    const char* preview = g_customRate || selected < 0 ? "Custom" : kFrameRates[selected].label;
    if (ImGui::BeginCombo("##FrameRateLimit", preview)) {
        for (int i = 0; i < static_cast<int>(std::size(kFrameRates)); ++i) {
            const bool itemSelected = !g_customRate && selected == i;
            if (ImGui::Selectable(kFrameRates[i].label, itemSelected)) {
                g_settings.frameRateLimit = kFrameRates[i].rate;
                g_customRate = false;
                selected = i;
                SaveSettings();
            }
            if (itemSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Custom", g_customRate)) g_customRate = true;
        if (g_customRate) ImGui::SetItemDefaultFocus();
        ImGui::EndCombo();
    }
}

void DrawCustomFrameRate() {
    int rate = static_cast<int>(g_settings.frameRateLimit == 0
        ? 60 : g_settings.frameRateLimit);
    if (ImGui::InputInt("##CustomFrameRate", &rate, 1, 10)) {
        g_settings.frameRateLimit = static_cast<unsigned>(
            std::clamp(rate, 15, 1000));
        g_editPending = true;
        PreviewSettings();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) FinishPerformanceEdit();
}

} // namespace

void FinishPerformanceEdit() {
    if (g_editPending) SaveSettings();
}

void DrawPerformancePanel() {
    EnsureSettings();
    if (widgets::BeginSettingsTable("##PerformanceSettings")) {
        if (!IsLightningReturns()) {
            widgets::SettingRow("Frame rate");
            DrawFrameRate();
            if (g_customRate) {
                widgets::SettingRow("Custom limit");
                DrawCustomFrameRate();
            }
        }

        if (!IsLightningReturns()) {
            widgets::SettingRow("VSync");
            int vsync = static_cast<int>(g_settings.vsyncMode);
            if (ImGui::Combo("##VSync", &vsync, kVSyncLabels, static_cast<int>(std::size(kVSyncLabels)))) {
                g_settings.vsyncMode = static_cast<settings::VSyncMode>(vsync);
                SaveSettings();
            }
        }

        const bool hasHighFpsCorrectness =
            game::Supports(game::Capability::HighFpsFacialTiming) ||
            game::CurrentProfile().title == game::Title::FinalFantasyXIII2;
        if (hasHighFpsCorrectness &&
            (IsLightningReturns() || g_settings.frameRateLimit == 0 ||
             g_settings.frameRateLimit > 30)) {
            const bool xiii2 = game::CurrentProfile().title ==
                               game::Title::FinalFantasyXIII2;
            const bool lr = game::CurrentProfile().title ==
                            game::Title::LightningReturns;
            widgets::SettingRow(
                lr ? "Facial animation timing" : "High-FPS timing fixes",
                lr
                    ? "Keeps facial morphs and smoothing correct above 60 FPS"
                    : xiii2
                    ? "Keeps 29.97 Hz simulation timing independent of presentation rate"
                    : "Corrects facial transition timing above 30 FPS");
            if (ImGui::Checkbox(
                    "##HighFpsCorrectness",
                    &g_settings.highFpsCorrectness)) {
                SaveSettings();
            }
        }
        widgets::EndSettingsTable();
    }

    ImGui::Spacing();
    if (!ImGui::CollapsingHeader("Advanced")) return;
    if (widgets::BeginSettingsTable("##AdvancedPerformanceSettings")) {
        widgets::SettingRow(
            "Renderer",
            "Automatic tries d3d9_chain.dll or ReShade\\d3d9.dll first, then the system-provided D3D9 backend. "
            "Local DXVK uses dxvk.dll next to NovaFix. Takes effect after restart");
        int backend = static_cast<int>(g_settings.d3d9Backend);
        if (ImGui::Combo("##D3D9Backend", &backend, kD3D9BackendLabels,
                         static_cast<int>(std::size(kD3D9BackendLabels)))) {
            g_settings.d3d9Backend = static_cast<settings::D3D9Backend>(backend);
            SaveSettings();
        }

        if (game::CurrentProfile().title != game::Title::LightningReturns) {
            widgets::SettingRow("Triple buffering",
                                "On uses two back buffers. Takes effect after restart");
            int triple = static_cast<int>(g_settings.tripleBuffering);
            if (ImGui::Combo("##TripleBuffering", &triple, kTripleBufferingLabels,
                             static_cast<int>(std::size(kTripleBufferingLabels)))) {
                g_settings.tripleBuffering =
                    static_cast<settings::TripleBufferingMode>(triple);
                SaveSettings();
            }
        }

        if (game::CurrentProfile().title ==
            game::Title::FinalFantasyXIII2) {
            settings::GraphicsSettings graphics = settings::Stored();
            widgets::SettingRow(
                "Optimize shadow pipeline",
                "Uses fixed-function depth and renders shadow cascades directly into the final atlas. Takes effect after restart");
            if (ImGui::Checkbox(
                    "##XIII2OptimizeShadowPipeline",
                    &graphics.xiii2OptimizeShadowPipeline)) {
                settings::Save(graphics);
            }
            widgets::SettingRow(
                "Optimize state submission",
                "Drops exact duplicate Direct3D 9 state calls from the original renderer. Takes effect after restart");
            if (ImGui::Checkbox(
                    "##XIII2OptimizeStateSubmission",
                    &g_settings.xiii2OptimizeStateSubmission)) {
                SaveSettings();
            }
        }

        widgets::SettingRow("Stable scheduling",
                            "Raises game and render-thread priority and disables Windows power throttling. Ignored on Wine/Proton. Takes effect after restart");
        if (ImGui::Checkbox(
                "##SchedulingStability", &g_settings.schedulingStability)) {
            SaveSettings();
        }
        widgets::EndSettingsTable();
    }
}

} // namespace novafix::ui
