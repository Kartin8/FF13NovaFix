#include "game/titles/ff13_2/rendering/shadows/direct_atlas.h"

#include "diagnostics/log.h"
#include "game/titles/ff13_2/rendering/shadows/frame_runtime.h"
#include "game/titles/ff13_2/rendering/shadows/native_hooks.h"

namespace novafix::game::xiii2_shadow_direct_atlas {

void Pump() {
    xiii2_shadow_native_hooks::Pump();
}

bool Enable() {
    const bool installed = xiii2_shadow_native_hooks::Installed();
    if (!installed || !xiii2_shadow_native_hooks::FactoryObserved() ||
        xiii2_shadow_frame_runtime::FailedClosed()) {
        Log("XIII-2 cached direct atlas enable rejected: installed=%d factory=%d failed-closed=%d",
            installed, xiii2_shadow_native_hooks::FactoryObserved(),
            xiii2_shadow_frame_runtime::FailedClosed());
        return false;
    }
    xiii2_shadow_frame_runtime::Activate();
    return true;
}

Status Snapshot() {
    Status status{};
    status.installed = xiii2_shadow_native_hooks::Installed();
    status.enabled = xiii2_shadow_frame_runtime::Enabled();
    status.factoryObserved = xiii2_shadow_native_hooks::FactoryObserved();
    status.failedClosed = xiii2_shadow_frame_runtime::FailedClosed();
    return status;
}

bool SuppressNativePacking(void* source, void* destination) {
    return xiii2_shadow_frame_runtime::SuppressNativePacking(
        source, destination);
}

} // namespace novafix::game::xiii2_shadow_direct_atlas
