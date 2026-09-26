#pragma once

struct IDirect3DDevice9;

namespace novafix::game::lr_snapshot {

// Restores the offline Snapshot route only at the known call sites for this build
void Pump();

// Captures the first HUD-free frame after RequestSnap.
void OnPresent(IDirect3DDevice9* device);

} // namespace novafix::game::lr_snapshot
