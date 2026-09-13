#include "game/core/features.h"

#include "diagnostics/log.h"
#include "diagnostics/hook_profiler.h"
#include "compat/compatibility.h"
#include "game/core/catalog.h"
#include "game/core/game_profile.h"
#include "patch/registry.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace novafix::game::features {
namespace {

INIT_ONCE g_registryOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_activationOnce = INIT_ONCE_STATIC_INIT;
struct RegisteredInitializer {
    const char* name{};
    void (*function)(){};
    bool (*ready)(){};
};
std::vector<RegisteredInitializer> g_initializers;
struct RegisteredUpdate {
    void (*function)(){};
    hook_profiler::DynamicPoint timing;
};
std::vector<RegisteredUpdate> g_updates;
std::vector<void (*)()> g_topologyNotifications;
std::atomic_bool g_activated{false};

BOOL CALLBACK InitializeRegistry(PINIT_ONCE, PVOID, PVOID*) {
    const auto featureSet = adapters::FeaturesFor(CurrentProfile().title);
    g_initializers.reserve(featureSet.size());
    g_updates.reserve(featureSet.size());
    g_topologyNotifications.reserve(featureSet.size());
    const bool compatibilityAllowsChanges = compat::compatibility::AllowsNovaFixChanges();
    if (compatibilityAllowsChanges) {
        for (const adapters::FeatureDescriptor& feature : featureSet) {
            if (!Supports(feature.capability)) continue;
            if (feature.initialize) {
                g_initializers.push_back({
                    feature.name, feature.initialize, feature.ready});
            }
            if (feature.update) {
                std::string timingName = "feature.update.";
                timingName += feature.name;
                g_updates.push_back({
                    feature.update,
                    hook_profiler::RegisterDynamicPoint(timingName),
                });
            }
            if (feature.notifyDeviceTopologyChanged) {
                g_topologyNotifications.push_back(
                    feature.notifyDeviceTopologyChanged);
            }
        }
    }
    return TRUE;
}

void EnsureRegistry() {
    InitOnceExecuteOnce(
        &g_registryOnce, &InitializeRegistry, nullptr, nullptr);
}

BOOL CALLBACK ActivateRegistry(PINIT_ONCE, PVOID, PVOID*) {
    EnsureRegistry();
    LogInfo("Game hooks:");
    for (const RegisteredInitializer& initialize : g_initializers) {
        const unsigned failuresBefore =
            FailureMessageCountForCurrentThread();
        const unsigned errorsBefore =
            ErrorMessageCountForCurrentThread();
        const patch::registry::Summary patchesBefore =
            patch::registry::CurrentSummary();
        initialize.function();
        const patch::registry::Summary patchesAfter =
            patch::registry::CurrentSummary();
        const bool installedPatch =
            patchesAfter.applied > patchesBefore.applied ||
            patchesAfter.dormant > patchesBefore.dormant;
        const bool noErrors =
            ErrorMessageCountForCurrentThread() == errorsBefore;
        const bool succeeded = noErrors && (initialize.ready
            ? initialize.ready()
            : installedPatch ||
                FailureMessageCountForCurrentThread() == failuresBefore);
        LogInfo("%s - %s", initialize.name,
                succeeded ? "ok" : "not ok");
    }
    g_activated.store(true, std::memory_order_release);
    return TRUE;
}

} // namespace

void Activate() {
    if (g_activated.load(std::memory_order_acquire)) return;
    InitOnceExecuteOnce(
        &g_activationOnce, &ActivateRegistry, nullptr, nullptr);
}

void Pump() {
    Activate();
    {
        hook_profiler::Scope profile(
            hook_profiler::Point::FeatureUpdates);
        for (const RegisteredUpdate& update : g_updates) {
            hook_profiler::Scope updateTiming(update.timing);
            update.function();
        }
    }
    hook_profiler::ReportIfDue();
}

void NotifyDeviceTopologyChanged() {
    EnsureRegistry();
    for (const auto notify : g_topologyNotifications) notify();
}

} // namespace novafix::game::features
