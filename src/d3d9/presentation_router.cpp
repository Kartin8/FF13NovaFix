#include "d3d9/presentation_router.h"

#include "diagnostics/log.h"
#include "compat/compatibility.h"
#include "d3d9/device_recovery_policy.h"
#include "d3d9/redundant_state_filter.h"
#include "display/display_profile.h"
#include "game/core/features.h"
#include "game/core/game_profile.h"
#include "game/titles/lightning_returns/snapshot/mode_controller.h"
#include "game/shared/ui/startup_module_notification.h"
#include "notifications/sounds.h"
#include "overlay/overlay.h"
#include "patch/integrity_monitor.h"
#include "performance/frame_limiter.h"
#include "performance/frame_metrics.h"
#include "performance/scheduling_stability.h"

#include <atomic>

namespace novafix::d3d9::presentation {
namespace {

enum class SelectedPath : LONG { None, Device, SwapChain };

std::atomic<SelectedPath> g_selectedPath{SelectedPath::None};
std::atomic_bool g_ff13DeviceWorkSuspended{false};
thread_local bool g_insidePresent = false;

SelectedPath ToSelectedPath(Path path) {
    return path == Path::Device ? SelectedPath::Device : SelectedPath::SwapChain;
}

} // namespace

bool Begin(IDirect3DDevice9* device, HWND window, Path path) {
    const SelectedPath requested = ToSelectedPath(path);
    SelectedPath selected =
        g_selectedPath.load(std::memory_order_acquire);
    if (selected == SelectedPath::None) {
        if (g_selectedPath.compare_exchange_strong(
                selected, requested, std::memory_order_acq_rel)) {
            selected = requested;
            Log("Presentation path selected: %s",
                path == Path::Device ? "device" : "swap-chain");
        }
    }
    if (selected != requested || !device || g_insidePresent) return false;

    // The profile is immutable after bootstrap; avoid rediscovery on every Present
    static const game::Title title = game::CurrentProfile().title;
    const bool conservativeRecovery =
        title == game::Title::FinalFantasyXIII;
    const HRESULT cooperativeResult = conservativeRecovery
        ? device->TestCooperativeLevel() : D3D_OK;
    if (device_recovery::SuspendNovaFixWork(
            conservativeRecovery, cooperativeResult)) {
        if (!g_ff13DeviceWorkSuspended.exchange(
                true, std::memory_order_acq_rel)) {
            Log("XIII D3D9 work suspended while the device is lost: result=0x%08lX",
                static_cast<unsigned long>(cooperativeResult));
        }
        return false;
    }
    if (conservativeRecovery &&
        g_ff13DeviceWorkSuspended.exchange(false, std::memory_order_acq_rel)) {
        Log("XIII D3D9 work resumed after device recovery");
    }

    g_insidePresent = true;
    if (!overlay::PresentationWindow()) overlay::SelectPresentationWindow(window);
    display::ActivateInitialWindow(window);
    display::MaintainProfile(window);
    performance::BeginFrame(device, window);
    redundant_state_filter::BeginFrame(device);
    // Compatibility does not change after startup, so do not ask every frame
    static const bool compatibilityAllowsChanges =
        compat::compatibility::AllowsNovaFixChanges();
    if (compatibilityAllowsChanges) performance::PumpSchedulingStability();
    game::features::Pump();
    patch::integrity_monitor::PumpFallback();
    // LR Snapshot capture must run before NovaFix draws its own UI
    if (title == game::Title::LightningReturns) {
        game::lr_snapshot::OnPresent(device);
    }
    game::startup_module_notification::RetryDeferredInstallation();
    overlay::OnPresent(device);
    if (compatibilityAllowsChanges && title != game::Title::LightningReturns) {
        performance::PaceFrame(
            window, performance::ObservedRefreshRate());
    }
    return true;
}

void End(HRESULT result) {
    if (SUCCEEDED(result)) {
        notifications::sounds::OnSuccessfulPresent();
    }
    performance::EndFrame(result);
    g_insidePresent = false;
}

} // namespace novafix::d3d9::presentation
