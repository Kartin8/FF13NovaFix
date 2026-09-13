#pragma once

#include "patch/pointer_hook.h"

#include <cstddef>
#include <string_view>

namespace novafix {

inline bool ReplaceVtableSlot(std::string_view feature, void** vtable, size_t index,
                              void* replacement, void** original,
                              patch::registry::ClaimId* claim = nullptr) {
    if (!vtable) return false;
    return patch::InstallPointerHook(feature, &vtable[index], replacement, original,
                                     patch::registry::Kind::VtableHook, claim);
}

} // namespace novafix
