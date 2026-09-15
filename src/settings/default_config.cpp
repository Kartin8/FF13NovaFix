#include "settings/default_config.h"

#include "game/core/game_profile.h"
#include "settings/config_path.h"
#include "settings/graphics_settings.h"
#include "settings/input_settings.h"
#include "settings/named_values.h"
#include "settings/performance_settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace novafix::settings {
namespace {

void AppendLine(std::string& output, std::string_view line = {}) {
    output.append(line);
    output.append("\r\n");
}

void AppendValue(std::string& output, std::string_view key, unsigned value) {
    output.append(key);
    output.push_back('=');
    output.append(std::to_string(value));
    output.append("\r\n");
}

void AppendValue(std::string& output, std::string_view key,
                 const wchar_t* value) {
    output.append(key);
    output.push_back('=');
    while (*value) output.push_back(static_cast<char>(*value++));
    output.append("\r\n");
}

void AppendNamedValue(
    std::string& output, std::string_view key, unsigned value,
    std::span<const wchar_t* const> names) {
    if (value >= names.size()) return AppendValue(output, key, value);
    AppendValue(output, key, names[value]);
}

void AppendBoolean(std::string& output, std::string_view key,
                   bool value) {
    AppendValue(output, key, value ? L"On" : L"Off");
}

void AppendGraphics(std::string& output, game::Title title) {
    if (title != game::Title::FinalFantasyXIII &&
        title != game::Title::FinalFantasyXIII2) {
        return;
    }

    const GraphicsSettings value{};
    AppendLine(output, "[Graphics]");
    AppendLine(output);
    AppendLine(output, "; DISPLAY");
    AppendBoolean(output, "OverrideLauncherGraphics",
                  value.overrideLauncherGraphics);
    AppendLine(output, "; Windowed | Borderless | BorderlessFullscreen");
    AppendNamedValue(
        output, "DisplayMode", static_cast<unsigned>(value.displayMode),
        names::kDisplayMode);
    AppendLine(output, "; Automatic | 1 | 2 | 3...");
    AppendNamedValue(
        output, "Monitor", value.monitorIndex, names::kMonitor);
    AppendLine(output, "; Used only for Windowed and Borderless");
    AppendValue(output, "Width", value.width);
    AppendValue(output, "Height", value.height);

    AppendLine(output);
    AppendLine(output, "; IMAGE QUALITY");
    AppendLine(output, "; 512 | 1024 | 2048 | 4096 | 8192");
    AppendValue(output, "ShadowResolution", value.shadowResolution);
    AppendLine(output, "; 2 | 4 | 8 | 16");
    AppendValue(output, "MSAA", value.msaaSamples);
    AppendLine(output, "; 0 | 2 | 4 | 8 | 16");
    AppendValue(output, "AnisotropicFiltering", value.anisotropicFiltering);

    if (title == game::Title::FinalFantasyXIII2) {
        char bias[16]{};
        _snprintf_s(bias, sizeof(bias), _TRUNCATE, "%.2f",
                    value.xiii2MipLodBias);
        AppendLine(output, "; -1.00 = sharper | 0.00 = neutral | 1.00 = softer");
        output.append("XIII2MipLodBias=");
        AppendLine(output, bias);
    }

    AppendLine(output);
    AppendLine(output, "; RENDERING FIXES");
    if (title == game::Title::FinalFantasyXIII) {
        AppendBoolean(output, "HighResolutionScissorFix",
                      value.highResolutionScissorFix);
    }
    AppendBoolean(output, "OptimizeUiVertexBuffer",
                  value.optimizeUiVertexBuffer);
    AppendBoolean(output, "CorrectScreenSpaceAlignment",
                  value.correctScreenSpaceAlignment);
    if (title == game::Title::FinalFantasyXIII2) {
        AppendBoolean(output, "XIII2ShaderCorrections",
                      value.xiii2ShaderCorrections);
        AppendBoolean(output, "XIII2OptimizeShadowPipeline",
                      value.xiii2OptimizeShadowPipeline);
    }
    AppendLine(output);
}

void AppendPerformance(std::string& output, game::Title title) {
    const PerformanceSettings value{};
    AppendLine(output, "[Performance]");
    AppendLine(output);
    if (title != game::Title::LightningReturns) {
        AppendLine(output, "; OUTPUT");
        AppendLine(output, "; Unlimited | 15..1000");
        AppendNamedValue(
            output, "FrameRateLimit", value.frameRateLimit,
            names::kFrameRateLimit);
        AppendLine(output, "; GameDefault | Off | On");
        AppendNamedValue(
            output, "VSyncMode", static_cast<unsigned>(value.vsyncMode),
            names::kVSyncMode);
        AppendLine(output, "; Automatic | 24..1000");
        AppendNamedValue(
            output, "RefreshRate", value.refreshRate,
            names::kRefreshRate);
        AppendLine(output, "; Automatic | Off | On");
        AppendNamedValue(
            output, "TripleBuffering",
            static_cast<unsigned>(value.tripleBuffering),
            names::kTripleBuffering);
        AppendLine(output);
    }

    AppendLine(output, "; SYSTEM");
    AppendBoolean(output, "ShowFpsCounter", value.showFpsCounter);
    AppendBoolean(output, "SmartControllerHotplug",
                  value.smartControllerHotplug);
    AppendBoolean(output, "SchedulingStability",
                  value.schedulingStability);
    if (title == game::Title::FinalFantasyXIII2) {
        AppendBoolean(output, "XIII2OptimizeStateSubmission",
                      value.xiii2OptimizeStateSubmission);
    }
    AppendBoolean(output, "HighFpsCorrectness",
                  value.highFpsCorrectness);

    AppendLine(output);
    AppendLine(output, "; NOTIFICATIONS");
    AppendBoolean(output, "StartupSound", value.startupSound);
    AppendBoolean(output, "MenuSounds", value.menuSounds);
    AppendLine(output, "; Never | WhenChanged | EveryLaunch");
    AppendNamedValue(
        output, "LoadedModulesNotificationMode",
        static_cast<unsigned>(value.loadedModulesNotificationMode),
        names::kLoadedModulesNotificationMode);

    AppendLine(output);
    AppendLine(output, "; RENDERER");
    AppendLine(output, "; Automatic | System | LocalDXVK");
    AppendNamedValue(
        output, "D3D9Backend", static_cast<unsigned>(value.d3d9Backend),
        names::kD3D9Backend);
    AppendLine(output);
}

void AppendInput(std::string& output) {
    const InputSettings value{};
    AppendLine(output, "[Input]");
    AppendLine(output);
    AppendLine(output, "; CONTROLLER");
    AppendLine(output, "; Automatic | Controller | Keyboard");
    AppendNamedValue(
        output, "PromptMode", static_cast<unsigned>(value.promptMode),
        names::kPromptMode);
    AppendBoolean(output, "ControllerVibration",
                  value.controllerVibration);
    AppendLine(output, "; 10..200");
    AppendValue(output, "VibrationStrengthPercent",
                value.vibrationStrengthPercent);

    AppendLine(output);
    AppendLine(output, "; OVERLAY");
    AppendLine(output, "; Disabled | F8 | F9 | F10 | F11 | F12 | Grave | Insert | Home");
    AppendNamedValue(
        output, "OverlayHotkey",
        static_cast<unsigned>(value.overlayHotkey),
        names::kOverlayHotkey);
    AppendNamedValue(
        output, "AlternateOverlayHotkey",
        static_cast<unsigned>(value.alternateOverlayHotkey),
        names::kOverlayHotkey);
    AppendLine(output, "; Disabled | StickClicks | ViewMenu");
    AppendNamedValue(
        output, "OverlayGamepadHotkey",
        static_cast<unsigned>(value.overlayGamepadHotkey),
        names::kOverlayGamepadHotkey);
    AppendLine(output);
}

std::string BuildDefaultConfig(game::Title title) {
    if (title == game::Title::Unknown) return {};

    std::string output;
    output.reserve(2300);
    AppendLine(output, "; NovaFix settings");
    AppendLine(output, "; Boolean values: Off | On");
    AppendLine(output, "; Edit while the game is closed");
    AppendLine(output);
    AppendGraphics(output, title);
    AppendPerformance(output, title);
    AppendInput(output);
    AppendLine(output, "[Diagnostics]");
    AppendLine(output, "; Off, Error, Warning, Info, or Debug");
    AppendLine(output, "LogLevel=Info");
    return output;
}

bool WriteNewFile(const std::wstring& path, std::string_view contents) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
    }

    bool complete = true;
    const char* cursor = contents.data();
    std::size_t remaining = contents.size();
    while (remaining != 0) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(remaining, MAXDWORD));
        DWORD written{};
        if (!WriteFile(file, cursor, request, &written, nullptr) || !written) {
            complete = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }
    if (!CloseHandle(file)) complete = false;
    if (!complete) DeleteFileW(path.c_str());
    return complete;
}

} // namespace

bool EnsureDefaultConfig(game::Title title) {
    const std::wstring& path = storage::ConfigFile();
    if (path.empty()) return false;
    const std::string contents = BuildDefaultConfig(title);
    return !contents.empty() && WriteNewFile(path, contents);
}

} // namespace novafix::settings
