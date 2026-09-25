#pragma once

#include <windows.h>

namespace novafix::d3d9::backend {

HMODULE Module();
FARPROC Export(const char* name);
FARPROC ExportOrdinal(WORD ordinal);
bool IsDxvk();

} // namespace novafix::d3d9::backend
