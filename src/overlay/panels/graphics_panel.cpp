#include "overlay/panels/graphics_panel.h"

#include "d3d9/device_hooks.h"
#include "display/monitor_layout.h"
#include "game/core/game_profile.h"
#include "settings/graphics_settings.h"
#include "overlay/panels/settings_widgets.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace novafix::ui {
namespace {

settings::GraphicsSettings g_settings{};
bool g_settingsReady = false;
bool g_customResolution = false;
bool g_editPending = false;
std::vector<display::MonitorDescriptor> g_monitors;
std::vector<std::string> g_monitorLabels;
std::vector<const char*> g_monitorItems;
unsigned g_monitorGeneration{};
int g_lastMonitorFrame = -2;

void RefreshMonitors() {
    const int frame = ImGui::GetFrameCount();
    const unsigned generation = display::MonitorLayoutGeneration();
    const bool refresh = frame != g_lastMonitorFrame + 1 ||
                         generation != g_monitorGeneration;
    g_lastMonitorFrame = frame;
    if (!refresh) return;
    g_monitorGeneration = generation;
    g_monitors = display::EnumerateMonitors();
    g_monitorItems.clear();
    g_monitorLabels = {"Automatic"};
    g_monitorLabels.reserve(g_monitors.size() + 1);
    for (size_t i = 0; i < g_monitors.size(); ++i) {
        const LONG width = g_monitors[i].bounds.right - g_monitors[i].bounds.left;
        const LONG height = g_monitors[i].bounds.bottom - g_monitors[i].bounds.top;
        g_monitorLabels.push_back("Monitor " + std::to_string(i + 1) + " (" +
            std::to_string(width) + " x " + std::to_string(height) +
            (g_monitors[i].primary ? ", Primary)" : ")"));
    }
    g_monitorItems.reserve(g_monitorLabels.size());
    for (const std::string& label : g_monitorLabels) {
        g_monitorItems.push_back(label.c_str());
    }
}

constexpr const char* kDisplayModes[]{
    "Windowed",
    "Borderless",
    "Borderless fullscreen",
};

struct Resolution {
    unsigned width;
    unsigned height;
    const char* label;
};

constexpr Resolution kResolutions[]{
    {1280, 720, "1280 x 720"},
    {1600, 900, "1600 x 900"},
    {1920, 1080, "1920 x 1080"},
    {2560, 1440, "2560 x 1440"},
    {3440, 1440, "3440 x 1440"},
    {3840, 2160, "3840 x 2160"},
};

int ResolutionIndex(const settings::GraphicsSettings& value) {
    for (int i = 0; i < static_cast<int>(std::size(kResolutions)); ++i) {
        if (kResolutions[i].width == value.width && kResolutions[i].height == value.height) return i;
    }
    return -1;
}

void EnsureSettings() {
    if (g_settingsReady) return;
    g_settings = settings::Stored();
    g_customResolution = ResolutionIndex(g_settings) < 0;
    g_settingsReady = true;
}

void PreviewLiveSettings() {
    settings::PreviewGraphics(g_settings);
    d3d9::device_hooks::PreviewTextureFiltering(
        g_settings.anisotropicFiltering, g_settings.xiii2MipLodBias);
}

bool SaveSettings() {
    g_editPending = false;
    settings::GraphicsSettings value = g_settings;
    value.xiii2OptimizeShadowPipeline =
        settings::Stored().xiii2OptimizeShadowPipeline;
    if (settings::Save(value)) {
        g_settings = value;
        return true;
    }
    g_settings = settings::Stored();
    g_customResolution = ResolutionIndex(g_settings) < 0;
    PreviewLiveSettings();
    return false;
}

void DrawResolutionCombo(bool desktopResolution) {
    int selected = ResolutionIndex(g_settings);
    const char* preview = desktopResolution ? "Desktop resolution" :
        (g_customResolution || selected < 0 ? "Custom" : kResolutions[selected].label);
    ImGui::BeginDisabled(desktopResolution);
    if (ImGui::BeginCombo("##Resolution", preview)) {
        for (int i = 0; i < static_cast<int>(std::size(kResolutions)); ++i) {
            const bool itemSelected = !g_customResolution && selected == i;
            if (ImGui::Selectable(kResolutions[i].label, itemSelected)) {
                g_settings.width = kResolutions[i].width;
                g_settings.height = kResolutions[i].height;
                g_customResolution = false;
                selected = i;
                SaveSettings();
            }
            if (itemSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Custom", g_customResolution)) {
            g_customResolution = true;
        }
        if (g_customResolution) ImGui::SetItemDefaultFocus();
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

void DrawCustomResolution() {
    int width = static_cast<int>(g_settings.width);
    int height = static_cast<int>(g_settings.height);
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth((available - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    if (ImGui::InputInt("##CustomWidth", &width, 0, 0)) {
        g_settings.width = static_cast<unsigned>(std::max(width, 640));
        g_editPending = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) FinishGraphicsEdit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputInt("##CustomHeight", &height, 0, 0)) {
        g_settings.height = static_cast<unsigned>(std::max(height, 480));
        g_editPending = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) FinishGraphicsEdit();
}

void DrawGraphicsSettings() {
    const bool presentationAvailable = game::Supports(game::Capability::PresentationOverride);
    if (!presentationAvailable) {
        ImGui::TextDisabled("Graphics settings aren't available for this executable.");
    } else if (widgets::BeginSettingsTable("##GraphicsProfile")) {
        widgets::SettingRow("Override launcher graphics settings");
        if (ImGui::Checkbox(
                "##UseNovaFixProfile",
                &g_settings.overrideLauncherGraphics)) {
            SaveSettings();
        }
        widgets::EndSettingsTable();
    }

    if (presentationAvailable) {
      ImGui::Spacing();
      ImGui::BeginDisabled(!g_settings.overrideLauncherGraphics);
      if (widgets::BeginSettingsTable("##GraphicsSettings")) {
        widgets::SettingRow("Display mode", "Takes effect after restart");
        int displayMode = static_cast<int>(g_settings.displayMode);
        if (ImGui::Combo("##DisplayMode", &displayMode, kDisplayModes,
                         static_cast<int>(std::size(kDisplayModes)))) {
            g_settings.displayMode =
                static_cast<settings::DisplayMode>(displayMode);
            SaveSettings();
        }

        RefreshMonitors();
        if (g_monitors.size() > 1) {
            widgets::SettingRow(
                "Monitor",
                "Automatic uses the monitor containing the game window. Takes effect after restart");
            int monitorIndex = static_cast<int>(std::min<size_t>(g_settings.monitorIndex,
                                                                  g_monitors.size()));
            if (ImGui::Combo("##Monitor", &monitorIndex, g_monitorItems.data(),
                             static_cast<int>(g_monitorItems.size()))) {
                g_settings.monitorIndex = static_cast<unsigned>(monitorIndex);
                SaveSettings();
            }
        }

        const bool desktopResolution =
            g_settings.displayMode == settings::DisplayMode::BorderlessFullscreen;
        widgets::SettingRow("Resolution", "Takes effect after restart");
        DrawResolutionCombo(desktopResolution);

        if (g_customResolution && !desktopResolution) {
            widgets::SettingRow("Custom size");
            DrawCustomResolution();
        }

        constexpr unsigned shadows[]{512, 1024, 2048, 4096, 8192};
        constexpr const char* shadowLabels[]{"Low (512)", "Medium (1024)", "High (2048)",
                                             "Very high (4096)", "Extreme (8192)"};
        widgets::SettingRow("Shadow quality",
                            "8192 x 8192 shadows are very GPU-heavy, especially in XIII-2. Takes effect after restart");
        int shadowIndex = settings::ShadowLauncherIndex(
            g_settings.shadowResolution);
        if (ImGui::Combo("##ShadowQuality", &shadowIndex, shadowLabels,
                         static_cast<int>(std::size(shadowLabels)))) {
            g_settings.shadowResolution = shadows[shadowIndex];
            SaveSettings();
        }

        constexpr unsigned samples[]{2, 4, 8, 16};
        constexpr const char* msaaLabels[]{"2x", "4x", "8x", "16x"};
        widgets::SettingRow("Anti-aliasing",
                            "4x or 8x is recommended. 16x MSAA is much heavier on the GPU. Takes effect after restart");
        int msaaIndex = settings::MsaaLauncherIndex(g_settings.msaaSamples);
        if (ImGui::Combo("##AntiAliasing", &msaaIndex, msaaLabels,
                         static_cast<int>(std::size(msaaLabels)))) {
            g_settings.msaaSamples = samples[msaaIndex];
            SaveSettings();
        }
          widgets::EndSettingsTable();
      }
      ImGui::EndDisabled();
    }

    ImGui::Spacing();
    const bool hasRenderingFixes =
        game::Supports(game::Capability::HighResolutionScissor) ||
        game::Supports(game::Capability::UiVertexBufferOptimization) ||
        game::Supports(game::Capability::ScreenSpaceAlignment) ||
        game::Supports(game::Capability::TextureFiltering) ||
        game::Supports(game::Capability::ShaderCorrection);
    if (hasRenderingFixes && ImGui::CollapsingHeader("Rendering fixes")) {
        if (widgets::BeginSettingsTable("##AdvancedGraphics")) {
            if (game::Supports(game::Capability::TextureFiltering)) {
                constexpr unsigned filteringValues[]{0, 2, 4, 8, 16};
                constexpr const char* filteringLabels[]{
                    "Default", "2x", "4x", "8x", "16x"};
                int filteringIndex = 0;
                for (int index = 0; index < static_cast<int>(std::size(filteringValues)); ++index) {
                    if (filteringValues[index] ==
                        g_settings.anisotropicFiltering) {
                        filteringIndex = index;
                        break;
                    }
                }
                const bool xiii2 = game::CurrentProfile().title ==
                                   game::Title::FinalFantasyXIII2;
                widgets::SettingRow(
                    "Texture filtering",
                    "Sharper texture filtering with less shimmering");
                if (ImGui::Combo("##TextureFiltering", &filteringIndex, filteringLabels,
                                 static_cast<int>(std::size(filteringLabels)))) {
                    g_settings.anisotropicFiltering =
                        filteringValues[filteringIndex];
                    if (SaveSettings()) {
                        d3d9::device_hooks::PreviewTextureFiltering(
                            g_settings.anisotropicFiltering,
                            g_settings.xiii2MipLodBias);
                    }
                }
                if (xiii2 && g_settings.anisotropicFiltering > 1) {
                    widgets::SettingRow(
                        "Mip LOD bias",
                        "Lower values sharpen textures; higher values reduce shimmering");
                    if (ImGui::SliderFloat(
                            "##XIII2MipLodBias", &g_settings.xiii2MipLodBias,
                            -1.0f, 1.0f, "%+.2f",
                            ImGuiSliderFlags_AlwaysClamp)) {
                        g_settings.xiii2MipLodBias =
                            std::round(g_settings.xiii2MipLodBias * 100.0f) /
                            100.0f;
                        g_editPending = true;
                        settings::PreviewGraphics(g_settings);
                        d3d9::device_hooks::PreviewXiii2MipLodBias(
                            g_settings.xiii2MipLodBias);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) FinishGraphicsEdit();
                }
            }
            if (game::Supports(game::Capability::HighResolutionScissor)) {
                widgets::SettingRow("Fix high-resolution UI",
                                    "Corrects hardcoded 1280 x 720 scissor rectangles that clip UI at higher resolutions");
                if (ImGui::Checkbox(
                        "##HighResolutionUI",
                        &g_settings.highResolutionScissorFix)) {
                    SaveSettings();
                }
            }
            if (game::Supports(game::Capability::UiVertexBufferOptimization)) {
                widgets::SettingRow("Optimize UI rendering",
                                    "Uses dynamic vertex buffers to avoid synchronization stalls in the 2D renderer. Takes effect after restart");
                if (ImGui::Checkbox(
                        "##OptimizeUiVertexBuffer",
                        &g_settings.optimizeUiVertexBuffer)) {
                    SaveSettings();
                }
            }
            if (game::Supports(game::Capability::ScreenSpaceAlignment)) {
                widgets::SettingRow("Correct screen alignment",
                                    "Corrects the hardcoded 720p half-pixel offset at higher resolutions");
                if (ImGui::Checkbox(
                        "##CorrectScreenSpaceAlignment",
                        &g_settings.correctScreenSpaceAlignment)) {
                    SaveSettings();
                }
            }
            if (game::Supports(game::Capability::ShaderCorrection)) {
                widgets::SettingRow(
                    "Cutout shadows",
                    "Makes shadows respect texture transparency. Takes effect after restart");
                if (ImGui::Checkbox(
                        "##XIII2ShaderCorrections",
                        &g_settings.xiii2ShaderCorrections)) {
                    SaveSettings();
                }
            }
            widgets::EndSettingsTable();
        }
    }
}

} // namespace

void FinishGraphicsEdit() {
    if (g_editPending) SaveSettings();
}

void DrawGraphicsPanel() {
    EnsureSettings();
    DrawGraphicsSettings();
}

} // namespace novafix::ui
