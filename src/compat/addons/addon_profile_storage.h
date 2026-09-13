#pragma once

#include <windows.h>

#include <string_view>

namespace novafix::compat::addon_profile_storage {

unsigned Install(HMODULE module, std::string_view ownerName);

} // namespace novafix::compat::addon_profile_storage
