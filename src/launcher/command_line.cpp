#include "launcher/command_line.h"

#include "patch/iat_hook.h"
#include "diagnostics/log.h"
#include "display/monitor_layout.h"
#include "game/core/game_profile.h"
#include "launcher/command_line_rewriter.h"
#include "settings/game_settings.h"
#include "settings/graphics_settings.h"

#include <windows.h>

#include <string>

namespace novafix::launcher {
namespace {

using GetCommandLineAFn = LPSTR (WINAPI*)();
using GetCommandLineWFn = LPWSTR (WINAPI*)();

GetCommandLineAFn g_getCommandLineA = nullptr;
GetCommandLineWFn g_getCommandLineW = nullptr;
INIT_ONCE g_ansiOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_wideOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_installOnce = INIT_ONCE_STATIC_INIT;
std::string g_ansiCommandLine;
std::wstring g_wideCommandLine;

BOOL CALLBACK BuildAnsiCommandLine(PINIT_ONCE, PVOID, PVOID*) {
    const char* original = g_getCommandLineA ? g_getCommandLineA() : nullptr;
    if (!original) return FALSE;
    const game::Title title = game::CurrentProfile().title;
    const bool rewriteGraphics =
        game::Supports(game::Capability::LauncherGraphics) &&
        settings::OverridesLauncherGraphics();
    const bool rewriteLanguages =
        (title == game::Title::FinalFantasyXIII ||
         title == game::Title::FinalFantasyXIII2) &&
        settings::OverridesGameLanguage();
    if (!rewriteGraphics && !rewriteLanguages) {
        g_ansiCommandLine = original;
        return TRUE;
    }
    const settings::GraphicsSettings graphics = settings::Active();
    const settings::GameSettings gameSettings = settings::ActiveGame();
    const SIZE monitor = rewriteGraphics
        ? display::ConfiguredMonitorSize(graphics.monitorIndex) : SIZE{};
    g_ansiCommandLine = RewriteLaunchOptions(
        original, graphics, gameSettings,
        static_cast<unsigned>(monitor.cx),
        static_cast<unsigned>(monitor.cy), rewriteGraphics,
        rewriteLanguages);
    const bool desktopSized =
        graphics.displayMode == settings::DisplayMode::BorderlessFullscreen;
    Log("Launch arguments applied: type=ANSI graphics=%u mode=%u render=%ux%u interface-language=%u voice-language=%u",
        rewriteGraphics ? 1u : 0u,
        static_cast<unsigned>(graphics.displayMode),
        desktopSized && monitor.cx > 0 ? static_cast<unsigned>(monitor.cx) : graphics.width,
        desktopSized && monitor.cy > 0 ? static_cast<unsigned>(monitor.cy) : graphics.height,
        static_cast<unsigned>(gameSettings.interfaceLanguage),
        static_cast<unsigned>(gameSettings.voiceLanguage));
    return TRUE;
}

BOOL CALLBACK BuildWideCommandLine(PINIT_ONCE, PVOID, PVOID*) {
    const wchar_t* original = g_getCommandLineW ? g_getCommandLineW() : nullptr;
    if (!original) return FALSE;
    const game::Title title = game::CurrentProfile().title;
    const bool rewriteGraphics =
        game::Supports(game::Capability::LauncherGraphics) &&
        settings::OverridesLauncherGraphics();
    const bool rewriteLanguages =
        (title == game::Title::FinalFantasyXIII ||
         title == game::Title::FinalFantasyXIII2) &&
        settings::OverridesGameLanguage();
    if (!rewriteGraphics && !rewriteLanguages) {
        g_wideCommandLine = original;
        return TRUE;
    }
    const settings::GraphicsSettings graphics = settings::Active();
    const settings::GameSettings gameSettings = settings::ActiveGame();
    const SIZE monitor = rewriteGraphics
        ? display::ConfiguredMonitorSize(graphics.monitorIndex) : SIZE{};
    g_wideCommandLine = RewriteLaunchOptions(
        original, graphics, gameSettings,
        static_cast<unsigned>(monitor.cx),
        static_cast<unsigned>(monitor.cy), rewriteGraphics,
        rewriteLanguages);
    const bool desktopSized =
        graphics.displayMode == settings::DisplayMode::BorderlessFullscreen;
    Log("Launch arguments applied: type=wide graphics=%u mode=%u render=%ux%u interface-language=%u voice-language=%u",
        rewriteGraphics ? 1u : 0u,
        static_cast<unsigned>(graphics.displayMode),
        desktopSized && monitor.cx > 0
            ? static_cast<unsigned>(monitor.cx) : graphics.width,
        desktopSized && monitor.cy > 0
            ? static_cast<unsigned>(monitor.cy) : graphics.height,
        static_cast<unsigned>(gameSettings.interfaceLanguage),
        static_cast<unsigned>(gameSettings.voiceLanguage));
    return TRUE;
}

LPSTR WINAPI GetCommandLineAHook() {
    InitOnceExecuteOnce(&g_ansiOnce, &BuildAnsiCommandLine, nullptr, nullptr);
    return g_ansiCommandLine.empty() && g_getCommandLineA ? g_getCommandLineA() : g_ansiCommandLine.data();
}

LPWSTR WINAPI GetCommandLineWHook() {
    InitOnceExecuteOnce(&g_wideOnce, &BuildWideCommandLine, nullptr, nullptr);
    return g_wideCommandLine.empty() && g_getCommandLineW ? g_getCommandLineW() : g_wideCommandLine.data();
}

template <typename Function>
bool PatchFromKnownKernelImports(const char* functionName, void* hook, Function* original) {
    constexpr const char* modules[]{
        "KERNEL32.dll",
        "api-ms-win-core-processenvironment-l1-1-0.dll",
        "api-ms-win-core-processenvironment-l1-2-0.dll",
    };
    for (const char* module : modules) {
        if (PatchExecutableImport(module, functionName, hook, reinterpret_cast<void**>(original))) return true;
    }
    return false;
}

} // namespace

BOOL CALLBACK Install(PINIT_ONCE, PVOID, PVOID*) {
    (void)PatchFromKnownKernelImports(
        "GetCommandLineA", reinterpret_cast<void*>(&GetCommandLineAHook),
        &g_getCommandLineA);
    (void)PatchFromKnownKernelImports(
        "GetCommandLineW", reinterpret_cast<void*>(&GetCommandLineWHook),
        &g_getCommandLineW);
    return TRUE;
}

void InstallCommandLineHooks() {
    InitOnceExecuteOnce(&g_installOnce, &Install, nullptr, nullptr);
}

} // namespace novafix::launcher
