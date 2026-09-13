#include "compat/compatibility.h"

#include "diagnostics/log.h"
#include "common/module_path.h"
#include "settings/config_path.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace novafix::compat::compatibility {
namespace {

constexpr std::uint64_t kMaximumScannedDllSize = 128ull * 1024ull * 1024ull;

INIT_ONCE g_scanOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Detection> g_blocked;
std::vector<Detection> g_active;
std::atomic_bool g_allowsChanges{true};

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && !right.empty() && path::Equivalent(left, right);
}

bool IsWithinDirectory(const std::wstring& filePath, const std::wstring& directory) {
    if (filePath.size() <= directory.size() || directory.empty() ||
        _wcsnicmp(filePath.c_str(), directory.c_str(), directory.size()) != 0) {
        return false;
    }
    const wchar_t separator = filePath[directory.size()];
    return separator == L'\\' || separator == L'/';
}

bool ContainsPath(const std::vector<Detection>& values, const std::wstring& candidate) {
    return std::any_of(values.begin(), values.end(), [&](const Detection& value) {
        return SamePath(value.path, candidate);
    });
}

foreign_fix::Product ClassifyFile(const std::wstring& filePath) {
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return foreign_fix::Product::None;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaximumScannedDllSize) {
        CloseHandle(file);
        return foreign_fix::Product::None;
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD total = 0;
    while (total < bytes.size()) {
        const DWORD remaining = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - total, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + total, remaining, &read, nullptr) || read == 0) break;
        total += read;
    }
    CloseHandle(file);
    if (total != bytes.size()) return foreign_fix::Product::None;
    return foreign_fix::Classify(bytes);
}

void Record(std::vector<Detection>& destination, foreign_fix::Product product,
            const std::wstring& filePath) {
    if (product == foreign_fix::Product::None || filePath.empty() ||
        ContainsPath(destination, filePath)) {
        return;
    }
    destination.push_back({product, filePath});
}

BOOL CALLBACK ScanLoadedModules(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring ownPath = path::ModuleFile(
        reinterpret_cast<HMODULE>(&__ImageBase));
    const std::wstring& moduleDirectory =
        settings::storage::ModuleDirectory();

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(snapshot, &module)) {
            do {
                const std::wstring modulePath = module.szExePath;
                if (SamePath(modulePath, ownPath) ||
                    !IsWithinDirectory(modulePath, moduleDirectory)) {
                    continue;
                }
                const foreign_fix::Product product = ClassifyFile(modulePath);
                if (product == foreign_fix::Product::None) continue;
                AcquireSRWLockExclusive(&g_lock);
                Record(g_active, product, modulePath);
                ReleaseSRWLockExclusive(&g_lock);
            } while (Module32NextW(snapshot, &module));
        }
        CloseHandle(snapshot);
    }

    AcquireSRWLockShared(&g_lock);
    const bool active = !g_active.empty();
    ReleaseSRWLockShared(&g_lock);
    g_allowsChanges.store(!active, std::memory_order_release);

    if (active) {
        LogWarning("FF13Fix detected: overlapping NovaFix features disabled");
    }
    return TRUE;
}

} // namespace

static bool ShouldBlockFile(const std::wstring& filePath, const char* kind) {
    if (filePath.empty()) return false;
    const foreign_fix::Product product = ClassifyFile(filePath);
    if (product == foreign_fix::Product::None) return false;

    AcquireSRWLockExclusive(&g_lock);
    Record(g_blocked, product, filePath);
    ReleaseSRWLockExclusive(&g_lock);
    Log("Competing %s blocked: product=%s path=%ls",
        kind, foreign_fix::ProductName(product), filePath.c_str());
    return true;
}

bool ShouldBlockBackend(const std::wstring& filePath) {
    return ShouldBlockFile(filePath, "D3D9 backend");
}

bool ShouldBlockExtraLibrary(const std::wstring& filePath) {
    return ShouldBlockFile(filePath, "add-on");
}

void Initialize() {
    InitOnceExecuteOnce(&g_scanOnce, &ScanLoadedModules, nullptr, nullptr);
}

bool AllowsNovaFixChanges() {
    Initialize();
    return g_allowsChanges.load(std::memory_order_acquire);
}

Report CurrentReport() {
    Initialize();
    AcquireSRWLockShared(&g_lock);
    Report report{g_blocked, g_active};
    ReleaseSRWLockShared(&g_lock);
    return report;
}

} // namespace novafix::compat::compatibility
