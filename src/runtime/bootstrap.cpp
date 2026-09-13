#include "runtime/bootstrap.h"

#include "diagnostics/log.h"
#include "compat/compatibility.h"
#include "compat/addons/addon_loader.h"
#include "compat/addons/addon_storage.h"
#include "compat/modules/loaded_modules.h"
#include "compat/platform.h"
#include "compat/addons/plugin_controls.h"
#include "compat/addons/native_notifications.h"
#include "diagnostics/crash_dumps.h"
#include "display/dpi_awareness.h"
#include "game/titles/ff13/rendering/nautilus_crash_fix.h"
#include "game/titles/ff13/ui/addon_notification.h"
#include "game/core/game_profile.h"
#include "game/titles/ff13_2/ui/addon_notification.h"
#include "game/titles/lightning_returns/snapshot/mode_controller.h"
#include "game/titles/lightning_returns/ui/addon_notification.h"
#include "game/steam/steam_cloud_safety.h"
#include "game/titles/ff13_2/rendering/shadows/direct_atlas.h"
#include "game/titles/ff13_2/rendering/shadows/fixed_depth.h"
#include "game/titles/ff13_2/rendering/shadows/optimization.h"
#include "input/direct_input/direct_input.h"
#include "input/win32_input.h"
#include "launcher/command_line.h"
#include "patch/integrity_monitor.h"
#include "runtime/restart.h"
#include "runtime/early_d3d_dispatch.h"
#include "settings/graphics_settings.h"
#include "settings/config_path.h"
#include "settings/default_config.h"
#include "settings/input_settings.h"
#include "settings/performance_settings.h"
#include "version.h"

#include <windows.h>

#include <atomic>
#include <iterator>
#include <string_view>

namespace novafix::runtime {
namespace {

INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_helper{false};
std::atomic_bool g_configReady{true};
std::atomic<game::Title> g_attachedTitle{game::Title::Unknown};
std::atomic<HMODULE> g_attachedModule{};

game::Title AttachedTitle() {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable,
                                            static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable)) return game::Title::Unknown;
    return game::IdentifyTitle(std::wstring_view(executable, length));
}

NovaFixPluginGame PluginGame(game::Title title) {
    switch (title) {
    case game::Title::FinalFantasyXIII: return NOVAFIX_PLUGIN_GAME_FF13;
    case game::Title::FinalFantasyXIII2: return NOVAFIX_PLUGIN_GAME_FF13_2;
    case game::Title::LightningReturns:
        return NOVAFIX_PLUGIN_GAME_LIGHTNING_RETURNS;
    case game::Title::Unknown: return NOVAFIX_PLUGIN_GAME_UNKNOWN;
    }
    return NOVAFIX_PLUGIN_GAME_UNKNOWN;
}

void PrepareLoadedModulesNotification(game::Title title) {
    const auto mode = settings::ActivePerformance()
                          .loadedModulesNotificationMode;
    if (mode == settings::LoadedModulesNotificationMode::Never) return;

    const auto& report = compat::loaded_modules::CurrentReport();
    if (report.entries.empty()) return;

    const std::wstring signature = compat::loaded_modules::Signature(report);
    if (mode == settings::LoadedModulesNotificationMode::WhenChanged &&
        settings::IsLoadedModulesNotificationSignatureCurrent(signature)) {
        return;
    }

    std::wstring persisted =
        mode == settings::LoadedModulesNotificationMode::WhenChanged
            ? signature : std::wstring{};
    std::wstring message = compat::loaded_modules::NotificationMessage(report);
    if (title == game::Title::LightningReturns) {
        game::lr_addon_notification::Queue(
            std::move(message), std::move(persisted));
    } else if (title == game::Title::FinalFantasyXIII) {
        game::ff13_addon_notification::Queue(
            std::move(message), std::move(persisted));
    } else if (title == game::Title::FinalFantasyXIII2) {
        game::xiii2_addon_notification::Queue(
            std::move(message), std::move(persisted));
    }
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID parameter, PVOID*) {
    if (parameter) *static_cast<bool*>(parameter) = true;
    if (g_helper.load(std::memory_order_acquire)) return TRUE;
    const game::GameProfile& profile = game::CurrentProfile();
    const bool configReady = profile.title == game::Title::Unknown ||
        settings::EnsureDefaultConfig(profile.title);
    g_configReady.store(configReady, std::memory_order_release);
    SetLogLevelFromIni(settings::storage::ConfigFile().c_str());
    StartLogWriter();
    LogInfo("Loaded version %s", kVersion);
    if (!configReady) {
        LogWarning("Could not create or open NovaFix.ini");
    }
    compat::LogEnvironment();
    LogInfo("Detected %s (%s, %s)",
        profile.titleName, game::ReleaseChannelName(profile.releaseChannel),
        profile.buildName);
    Log("Game profile: support=%s capabilities=0x%016llX",
        game::SupportName(profile.support),
        static_cast<unsigned long long>(profile.capabilities));
    if (profile.imageValid) {
        Log("Executable fingerprint: machine=%04X pe=%04X timestamp=%08X image=%u text=%u hash=%016llX",
            profile.fingerprint.machine, profile.fingerprint.optionalHeaderMagic,
            profile.fingerprint.timeDateStamp, profile.fingerprint.imageSize,
            profile.fingerprint.textSize,
            static_cast<unsigned long long>(profile.fingerprint.textHash));
    } else {
        LogError("Could not read game executable: game fixes disabled");
    }
    if (profile.support == game::BuildSupport::RecognizedTitle) {
        LogWarning("Unsupported executable: game fixes disabled");
    } else if (profile.support == game::BuildSupport::CompatibleLayout) {
        LogWarning("Modified executable detected: each fix will validate its own patch site");
    }
    Log("Configuration storage: %ls", settings::storage::ConfigFile().c_str());
    g_attachedTitle.store(profile.title, std::memory_order_release);
    settings::SetLauncherIntegrationEnabled(
        profile.title == game::Title::FinalFantasyXIII);
    if (profile.title == game::Title::Unknown) return TRUE;

    if (compat::IsWine()) {
        Log("Native crash dump filter skipped on Wine/Proton");
    } else {
        diagnostics::crash_dumps::Install(
            g_attachedModule.load(std::memory_order_acquire));
    }

    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&Initialize), &pinned)) {
        LogWarning("Could not pin module: error=%lu", GetLastError());
    }

    const bool nativeNotificationsSupported =
        profile.title == game::Title::LightningReturns &&
        profile.support == game::BuildSupport::ExactBuild;
    compat::native_notifications::Configure(nativeNotificationsSupported);
    compat::plugin_controls::ConfigureHost(
        PluginGame(profile.title), profile.titleName, kVersion,
        nativeNotificationsSupported
            ? &compat::native_notifications::Queue : nullptr,
        nativeNotificationsSupported
            ? &compat::native_notifications::QueueEx : nullptr);

    // Load add-ons outside the loader lock, before normal hooks
    compat::addon_loader::SetReuseExistingDirectoryPolicy(
        profile.title == game::Title::LightningReturns);
    compat::addon_storage::Configure(settings::storage::ModuleDirectory());
    compat::addon_loader::Initialize();

    // Native startup controllers are created early; queue the notice before D3D hooks
    if (profile.title == game::Title::LightningReturns ||
        profile.title == game::Title::FinalFantasyXIII ||
        profile.title == game::Title::FinalFantasyXIII2) {
        PrepareLoadedModulesNotification(profile.title);
    }

    display::InitializeDpiAwareness();
    input::InstallWin32Hooks();
    input::InstallDirectInputHooks();
    if (profile.title == game::Title::FinalFantasyXIII) {
        game::ff13_nautilus_crash_fix::Install();
    } else if (profile.title == game::Title::FinalFantasyXIII2) {
        game::steam_cloud::InstallEarly();
        // The atlas hook must catch the startup allocation before Direct3DCreate9 returns
        if (compat::compatibility::AllowsNovaFixChanges()) {
            game::xiii2_shadow_fixed_depth::Pump();
            game::xiii2_shadow_direct_atlas::Pump();
            game::xiii2_shadow_optimization::Pump();
        }
    }
    patch::integrity_monitor::Start();
    return TRUE;
}

} // namespace

void Attach(HMODULE module) {
    g_attachedModule.store(module, std::memory_order_release);
    settings::storage::Configure(module);
    SetLogModuleHandle(module);
    const game::Title title = AttachedTitle();
    restart::SetModuleHandle(module);
    const bool helper = restart::IsHelperProcess();
    g_helper.store(helper, std::memory_order_release);
    if (helper) return;

    settings::SetGraphicsTitle(title);
    g_attachedTitle.store(title, std::memory_order_release);
    settings::SetPerformancePresentationControlsEnabled(
        title != game::Title::LightningReturns);
    settings::SetLauncherIntegrationEnabled(title == game::Title::FinalFantasyXIII);
    if (title != game::Title::Unknown) {
        // Install before the game can cache a dynamically resolved D3D factory
        // Logging remains buffered until Initialize runs outside the loader lock
        early_d3d_dispatch::Install();
    }
    if (title == game::Title::FinalFantasyXIII ||
        title == game::Title::FinalFantasyXIII2) {
        // Install before the entry point; Crystal Tools reads graphics arguments before D3D9 initialization
        launcher::InstallCommandLineHooks();
    }
}

void Detach() {
    // The module is pinned after initialization. Let the process reclaim it and
    // its optional exception filter at exit; restoring a foreign filter during
    // loader teardown can point at a module that has already detached
    g_attachedTitle.store(game::Title::Unknown, std::memory_order_release);
}

bool EnsureInitialized() {
    bool initializedHere = false;
    InitOnceExecuteOnce(
        &g_initializeOnce, &Initialize, &initializedHere, nullptr);
    return initializedHere;
}

bool ShouldInstallD3D9Hooks() {
    return g_attachedTitle.load(std::memory_order_acquire) != game::Title::Unknown;
}

} // namespace novafix::runtime
