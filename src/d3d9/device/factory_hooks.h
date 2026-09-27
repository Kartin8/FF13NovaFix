#pragma once

struct IDirect3D9;

namespace novafix::d3d9 {

void InstallFactoryHooks(IDirect3D9* d3d, bool extended);

} // namespace novafix::d3d9
