#pragma once

namespace novafix::compat::addon_loader {

// Reuse the first existing add-on directory instead of creating "addons"
// If none exists, "addons" is created. Set before Initialize()
void SetReuseExistingDirectoryPolicy(bool enabled);

// Call after DllMain; loaded modules remain until process exit
void Initialize();

unsigned LoadedCount();

} // namespace novafix::compat::addon_loader
