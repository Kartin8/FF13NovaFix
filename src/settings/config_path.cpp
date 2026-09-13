#include "settings/config_path.h"

#include "common/module_path.h"

#include <windows.h>

namespace novafix::settings::storage {
namespace {

HMODULE g_module = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
std::wstring g_directory;
std::wstring g_config;

BOOL CALLBACK Resolve(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring modulePath = path::ModuleFile(g_module);
    if (modulePath.empty()) return TRUE;
    g_directory = path::Directory(modulePath);
    if (g_directory.empty()) return TRUE;

    g_config = g_directory + L"\\NovaFix.ini";
    return TRUE;
}

void EnsureResolved() {
    InitOnceExecuteOnce(&g_once, &Resolve, nullptr, nullptr);
}

} // namespace

void Configure(HMODULE module) {
    if (!g_module) g_module = module;
}

const std::wstring& ModuleDirectory() {
    EnsureResolved();
    return g_directory;
}

const std::wstring& ConfigFile() {
    EnsureResolved();
    return g_config;
}

} // namespace novafix::settings::storage
