#include "runtime/bootstrap.h"

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        novafix::runtime::Attach(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        novafix::runtime::Detach();
    }
    return TRUE;
}
