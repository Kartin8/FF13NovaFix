#pragma once

#include <windows.h>

namespace novafix::d3d9::backend {

HMODULE Module();
FARPROC Export(const char* name);
FARPROC ExportOrdinal(WORD ordinal);

} // namespace novafix::d3d9::backend
