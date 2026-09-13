#pragma once

namespace novafix::game::xiii2_shadow_direct_atlas {

struct Status {
    bool installed{};
    bool enabled{};
    bool factoryObserved{};
    bool failedClosed{};
};

// Installs the exact-build cached-atlas bridge before Crystal Tools creates
// its shadow owner. The matching atlas depth attachment is captured during
// startup and activation occurs only after complete owner validation
void Pump();
bool Enable();
Status Snapshot();

// Called by the fixed-depth module's 0x8219E0 hook. Returns true only while
// both cascade wrappers are transactionally mapped to the final atlas and the
// native copy is therefore a proven self-copy
bool SuppressNativePacking(void* source, void* destination);

} // namespace novafix::game::xiii2_shadow_direct_atlas
