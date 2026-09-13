#pragma once

#include "patch/registry.h"

#include <string_view>

namespace novafix::patch {

bool InstallPointerHook(std::string_view feature, void** slot, void* replacement,
                        void** original, registry::Kind kind,
                        registry::ClaimId* claim = nullptr);

} // namespace novafix::patch
