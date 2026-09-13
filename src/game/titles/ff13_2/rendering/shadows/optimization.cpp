#include "game/titles/ff13_2/rendering/shadows/optimization.h"

#include "diagnostics/log.h"
#include "d3d9/alpha_shadow_variants.h"
#include "d3d9/shader_corrections.h"
#include "game/core/game_profile.h"
#include "game/titles/ff13_2/rendering/shadows/direct_atlas.h"
#include "game/titles/ff13_2/rendering/shadows/fixed_depth.h"
#include "settings/graphics_settings.h"

#include <windows.h>

#include <atomic>

namespace novafix::game::xiii2_shadow_optimization {
namespace {

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
std::atomic_bool g_requested{false};
std::atomic_bool g_producerConfigured{false};
std::atomic_bool g_activationComplete{false};

enum class ProducerResult {
    Pending,
    Ready,
    Failed,
};

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    if (CurrentProfile().title != Title::FinalFantasyXIII2) return TRUE;
    const bool configured =
        settings::Active().xiii2OptimizeShadowPipeline;
    g_requested.store(configured, std::memory_order_release);
    return TRUE;
}

void EnsureInitialized() {
    InitOnceExecuteOnce(&g_once, &Initialize, nullptr, nullptr);
}

ProducerResult ConfigureProducer() {
    using Mode = d3d9::shader_corrections::OpaqueShadowMode;

    if (!xiii2_shadow_fixed_depth::Available() ||
        d3d9::shader_corrections::OpaqueShadowModeRejected(
            Mode::FixedFunctionDepth)) {
        d3d9::shader_corrections::SetOpaqueShadowMode(Mode::Native);
        g_producerConfigured.store(false, std::memory_order_release);
        Log("XIII-2 combined shadow producer retained native: fixed-depth draw contract unavailable");
        return ProducerResult::Failed;
    }
    if (!d3d9::shader_corrections::OpaqueShadowModeAvailable(
            Mode::FixedFunctionDepth)) {
        return ProducerResult::Pending;
    }
    if (!xiii2_shadow_fixed_depth::Enable()) {
        return ProducerResult::Failed;
    }
    d3d9::shader_corrections::SetOpaqueShadowMode(
        Mode::FixedFunctionDepth);
    d3d9::alpha_shadow_variants::SetFixedDepthEnabled(true);
    g_producerConfigured.store(true, std::memory_order_release);
    return ProducerResult::Ready;
}

void ApplyRequestedState() {
    if (g_activationComplete.load(std::memory_order_acquire)) return;
    using Mode = d3d9::shader_corrections::OpaqueShadowMode;
    const bool requested = g_requested.load(std::memory_order_acquire);
    if (!requested) {
        g_activationComplete.store(true, std::memory_order_release);
        return;
    }

    if (!g_producerConfigured.load(std::memory_order_acquire) ||
        d3d9::shader_corrections::GetOpaqueShadowMode() !=
            Mode::FixedFunctionDepth ||
        !xiii2_shadow_fixed_depth::Enabled()) {
        const ProducerResult producer = ConfigureProducer();
        if (producer == ProducerResult::Pending) return;
        if (producer == ProducerResult::Failed) {
            g_activationComplete.store(true, std::memory_order_release);
            return;
        }
    }

    const xiii2_shadow_direct_atlas::Status atlas =
        xiii2_shadow_direct_atlas::Snapshot();
    if (!atlas.enabled && atlas.installed && atlas.factoryObserved &&
        !atlas.failedClosed) {
        xiii2_shadow_direct_atlas::Enable();
    }

    const xiii2_shadow_direct_atlas::Status updated =
        xiii2_shadow_direct_atlas::Snapshot();
    if (updated.enabled) {
        g_activationComplete.store(true, std::memory_order_release);
    }
    if (updated.failedClosed) {
        LogWarning("XIII-2 shadow atlas failed: using the original packing path");
        Log("XIII-2 fixed-depth shadows remain enabled after atlas failure");
        g_activationComplete.store(true, std::memory_order_release);
    }
}

} // namespace

void Pump() {
    EnsureInitialized();
    if (g_activationComplete.load(std::memory_order_acquire)) return;
    if (CurrentProfile().title != Title::FinalFantasyXIII2) return;
    ApplyRequestedState();
}

bool InstallationViable() {
    EnsureInitialized();
    if (!g_requested.load(std::memory_order_acquire) ||
        !g_activationComplete.load(std::memory_order_acquire)) {
        return true;
    }
    return g_producerConfigured.load(std::memory_order_acquire);
}

} // namespace novafix::game::xiii2_shadow_optimization
