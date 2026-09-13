#pragma once

#include <windows.h>

namespace novafix {

bool PatchExecutableImport(const char* dllName, const char* functionName,
                           void* replacement, void** original);
bool PatchExecutableImportOrdinal(const char* dllName, WORD ordinal,
                                  void* replacement, void** original);
bool PatchModuleImport(HMODULE module, const char* ownerName, const char* dllName,
                       const char* functionName, void* replacement, void** original);
bool PatchModuleImportOrdinal(HMODULE module, const char* ownerName,
                              const char* dllName, WORD ordinal,
                              void* replacement, void** original);

// Reads the current IAT target without replacing it
// Used to replay the first call through newly loaded add-ons
void* ReadExecutableImportTarget(const char* dllName,
                                 const char* functionName);

} // namespace novafix
