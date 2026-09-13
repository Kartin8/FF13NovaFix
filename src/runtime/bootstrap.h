#pragma once

#include <windows.h>

namespace novafix::runtime {

void Attach(HMODULE module);
void Detach();
// Returns true only for the call that performs initialization,
// allowing the first D3D dispatch to be replayed afterward
bool EnsureInitialized();
bool ShouldInstallD3D9Hooks();

} // namespace novafix::runtime
