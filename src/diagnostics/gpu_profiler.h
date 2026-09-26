#pragma once

struct IDirect3DDevice9;

namespace novafix::gpu_profiler {

// Debug-only, sampled D3D9 timestamp queries for commands submitted by
// NovaFix itself. Results are polled without flushing or waiting for the GPU.
void Poll(IDirect3DDevice9* device);
void BeginOverlay(IDirect3DDevice9* device);
void EndOverlay(IDirect3DDevice9* device);
void BeginShadowClear(IDirect3DDevice9* device);
void EndShadowClear(IDirect3DDevice9* device);
void Reset(IDirect3DDevice9* device);

} // namespace novafix::gpu_profiler
