#include "overlay/panels/diagnostics_panel.h"

#include "diagnostics/log.h"
#include "overlay/panels/settings_widgets.h"
#include "performance/frame_metrics.h"
#include "settings/config_path.h"
#include "settings/performance_settings.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace novafix::ui {
namespace {

constexpr const char* kLogLevelLabels[]{
    "Off",
    "Errors",
    "Warnings",
    "Info",
    "Debug",
};

void MetricCell(const char* label, const char* value) {
    ImGui::TextColored(ImVec4(0.72f, 0.72f, 0.72f, 1.0f), "%s", label);
    ImGui::TextUnformatted(value);
}

void DrawFrameMetrics() {
    const performance::FrameStatisticsSnapshot statistics =
        performance::CurrentFrameStatistics();
    if (!statistics.sampleCount || statistics.averageFrameMs <= 0.0f) {
        ImGui::TextDisabled("Collecting frame data...");
        return;
    }

    char averageFps[32]{};
    char frameTime[32]{};
    char onePercentLow[32]{};
    _snprintf_s(averageFps, sizeof(averageFps), _TRUNCATE, "%.1f",
                1000.0f / statistics.averageFrameMs);
    _snprintf_s(frameTime, sizeof(frameTime), _TRUNCATE, "%.2f ms",
                statistics.currentFrameMs);
    _snprintf_s(onePercentLow, sizeof(onePercentLow), _TRUNCATE, "%.1f",
                statistics.onePercentLowFps);

    if (ImGui::BeginTable(
            "##MetricCards", 3,
            ImGuiTableFlags_SizingStretchSame |
                ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        MetricCell("Average FPS", averageFps);
        ImGui::TableSetColumnIndex(1);
        MetricCell("Frame time", frameTime);
        ImGui::TableSetColumnIndex(2);
        MetricCell("1% low", onePercentLow);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    const float graphMaximum =
        std::clamp(statistics.worstFrameMs, 16.67f, 50.0f);
    const float graphHeight =
        90.0f * std::max(1.0f, ImGui::GetFontSize() / 15.0f);
    const float graphWidth = ImGui::GetContentRegionAvail().x;

    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled();
    ImGui::PlotLines(
        "##NovaFixFrametime", statistics.frameTimes.data(),
        static_cast<int>(statistics.sampleCount), 0, nullptr, 0.0f,
        graphMaximum, ImVec2(graphWidth, graphHeight));
    ImGui::EndDisabled();
    ImGui::PopStyleVar();

    ImGui::Text("Late frames: %u", statistics.missedFrames);
}

} // namespace

void DrawDiagnosticsPanel() {
    if (widgets::BeginSettingsTable("##DiagnosticsSettings")) {
        widgets::SettingRow(
            "Log level",
            "Use Debug for hook failures, timing issues, and performance diagnostics");
        int logLevel = static_cast<int>(StoredLogLevel());
        if (ImGui::Combo("##LogLevel", &logLevel, kLogLevelLabels,
                         static_cast<int>(std::size(kLogLevelLabels)))) {
            SaveLogLevelToIni(
                settings::storage::ConfigFile().c_str(),
                static_cast<LogLevel>(logLevel));
        }

        settings::PerformanceSettings performance =
            settings::StoredPerformance();
        widgets::SettingRow("Show FPS counter");
        if (ImGui::Checkbox(
                "##ShowFpsCounter", &performance.showFpsCounter)) {
            settings::SavePerformance(performance);
        }
        widgets::EndSettingsTable();
    }

    ImGui::Spacing();
    DrawFrameMetrics();

}

} // namespace novafix::ui
