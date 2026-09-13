#include "launcher/command_line.h"

#include "patch/iat_hook.h"
#include "diagnostics/log.h"
#include "display/monitor_layout.h"
#include "game/core/game_profile.h"
#include "launcher/command_line_rewriter.h"
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
    if (!game::Supports(game::Capability::LauncherGraphics) ||
        !settings::OverridesLauncherGraphics()) {
        g_ansiCommandLine = original;
        return TRUE;
    }
    const settings::GraphicsSettings graphics = settings::Active();
    const SIZE monitor = display::ConfiguredMonitorSize(graphics.monitorIndex);
    g_ansiCommandLine = RewriteGraphicsOptions(original, graphics,
                                                static_cast<unsigned>(monitor.cx),
                                                static_cast<unsigned>(monitor.cy));
    const bool desktopSized =
        graphics.displayMode == settings::DisplayMode::BorderlessFullscreen;
    Log("Graphics arguments applied: type=ANSI mode=%u render=%ux%u source=%s",
        static_cast<unsigned>(graphics.displayMode),
        desktopSized && monitor.cx > 0 ? static_cast<unsigned>(monitor.cx) : graphics.width,
        desktopSized && monitor.cy > 0 ? static_cast<unsigned>(monitor.cy) : graphics.height,
        game::CurrentProfile().title == game::Title::FinalFantasyXIII2
            ? "XIII-2 early command line" : "XIII early command line");
    return TRUE;
}

BOOL CALLBACK BuildWideCommandLine(PINIT_ONCE, PVOID, PVOID*) {
    const wchar_t* original = g_getCommandLineW ? g_getCommandLineW() : nullptr;
    if (!original) return FALSE;
    if (!game::Supports(game::Capability::LauncherGraphics) ||
        !settings::OverridesLauncherGraphics()) {
        g_wideCommandLine = original;
        return TRUE;
    }
    const settings::GraphicsSettings graphics = settings::Active();
    const SIZE monitor = display::ConfiguredMonitorSize(graphics.monitorIndex);
    g_wideCommandLine = RewriteGraphicsOptions(original, graphics,
                                                static_cast<unsigned>(monitor.cx),
                                                static_cast<unsigned>(monitor.cy));
    const bool desktopSized =
        graphics.displayMode == settings::DisplayMode::BorderlessFullscreen;
    Log("Graphics arguments applied: type=wide mode=%u render=%ux%u source=%s",
        static_cast<unsigned>(graphics.displayMode),
        desktopSized && monitor.cx > 0
            ? static_cast<unsigned>(monitor.cx) : graphics.width,
        desktopSized && monitor.cy > 0
            ? static_cast<unsigned>(monitor.cy) : graphics.height,
        game::CurrentProfile().title == game::Title::FinalFantasyXIII2
            ? "XIII-2 early command line" : "XIII early command line");
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
