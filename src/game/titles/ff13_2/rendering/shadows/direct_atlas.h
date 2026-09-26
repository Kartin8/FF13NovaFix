#pragma once

namespace novafix::game::xiii2_shadow_direct_atlas {

struct Status {
    bool installed{};
    bool enabled{};
    bool factoryObserved{};
    bool failedClosed{};
};

// Install the cached-atlas hooks before Crystal Tools creates the shadow owner
// Capture the matching depth surface during startup and enable only after the ownr is validated
void Pump();
bool Enable();
Status Snapshot();

// Called from the fixed-depth 0x8219E0 hook
// True when both cascade wrappers already point at the final atlas, so the native copy would copy the atlas onto itself
bool SuppressNativePacking(void* source, void* destination);

} // namespace novafix::game::xiii2_shadow_direct_atlas
