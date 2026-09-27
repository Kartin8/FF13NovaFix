#pragma once

namespace novafix::d3d9 {

struct DeviceHookPolicy;

namespace device_state_hooks {

void Install(void** vtable, const DeviceHookPolicy& policy);
void Reactivate(void** vtable);
void MarkDormant(void** vtable);

} // namespace device_state_hooks
} // namespace novafix::d3d9
