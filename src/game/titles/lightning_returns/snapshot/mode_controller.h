#pragma once

struct IDirect3DDevice9;

namespace novafix::game::lr_snapshot {

// Restores LR's native offline Snapshot route, including its
// pause-menu prompt and original game input. Installation is exact-build and
// call-site scoped
void Pump();

// Captures the first fully rendered frame after SnsCameraMode has completed
// its native RequestSnap transition and hidden the photo-mode HUD
void OnPresent(IDirect3DDevice9* device);

} // namespace novafix::game::lr_snapshot
