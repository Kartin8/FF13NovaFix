#pragma once

#include <windows.h>

#include <string>

namespace novafix::settings::storage {

// Call during process attach before settings are first accessed
// Repeated calls with the same module are harmless
void Configure(HMODULE module);
const std::wstring& ModuleDirectory();
const std::wstring& ConfigFile();

} // namespace novafix::settings::storage
