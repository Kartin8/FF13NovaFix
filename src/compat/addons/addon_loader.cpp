#include "compat/addons/addon_loader.h"

#include "diagnostics/log.h"
#include "common/module_path.h"
#include "compat/addons/addon_directories.h"
#include "compat/addons/addon_storage.h"
#include "compat/compatibility.h"
#include "compat/addons/plugin_controls.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <string_view>
#include <tlhelp32.h>
#include <vector>

namespace novafix::compat::addon_loader {
namespace {

struct Candidate {
    std::wstring path;
    std::wstring relativePath;
    std::wstring orderPath;
    size_t directoryIndex{};
};

struct FileIdentity {
    DWORD volumeSerial{};
    DWORD indexHigh{};
    DWORD indexLow{};
    bool valid{};
};

enum class Status {
    Loaded,
    AlreadyLoaded,
    SkippedDuplicate,
    SkippedReserved,
    SkippedCompetingFix,
    Failed,
};

struct Report {
    std::wstring directory;
    unsigned loaded{};
    unsigned skipped{};
    unsigned failed{};
};

INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;
Report g_report;
std::atomic_bool g_reuseExistingDirectory{false};

HMODULE OwnModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&g_loadOnce), &module);
    return module;
}

std::wstring Join(std::wstring_view left, std::wstring_view right) {
    if (left.empty()) return std::wstring(right);
    std::wstring result(left);
    if (result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
    result.append(right);
    return result;
}

bool HasLoadableExtension(std::wstring_view name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    const std::wstring extension(name.substr(dot));
    return _wcsicmp(extension.c_str(), L".dll") == 0 ||
           _wcsicmp(extension.c_str(), L".asi") == 0;
}

bool IsReservedRenderer(std::wstring_view relativePath) {
    const size_t separator = relativePath.find_last_of(L"\\/");
    const std::wstring name(relativePath.substr(
        separator == std::wstring_view::npos ? 0 : separator + 1));
    constexpr std::array<const wchar_t*, 4> reserved{
        L"d3d9.dll", L"d3d9_chain.dll", L"dxvk.dll", L"ReShade32.dll"};
    return std::any_of(reserved.begin(), reserved.end(), [&](const wchar_t* item) {
        return _wcsicmp(name.c_str(), item) == 0;
    });
}

void Collect(const std::wstring& directory, const std::wstring& relative,
             std::wstring_view directoryName, size_t directoryIndex,
             std::vector<Candidate>* output) {
    if (!output) return;
    const std::wstring pattern = Join(directory, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;

    do {
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..") continue;
        const std::wstring pathName = Join(directory, name);
        const std::wstring relativeName = relative.empty()
            ? std::wstring(name) : Join(relative, name);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Do not recurse into reparse points; they may escape the add-on tree or form cycles
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                Collect(pathName, relativeName, directoryName,
                        directoryIndex, output);
            }
            continue;
        }
        if (HasLoadableExtension(name)) {
            output->push_back({
                pathName,
                Join(directoryName, relativeName),
                relativeName,
                directoryIndex,
            });
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
}

FileIdentity IdentityOf(const std::wstring& filePath) {
    HANDLE file = CreateFileW(
        filePath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION information{};
    const bool queried = GetFileInformationByHandle(file, &information) != FALSE;
    CloseHandle(file);
    if (!queried) return {};
    return {information.dwVolumeSerialNumber, information.nFileIndexHigh,
            information.nFileIndexLow, true};
}

bool SameIdentity(const FileIdentity& left, const FileIdentity& right) {
    return left.valid && right.valid &&
           left.volumeSerial == right.volumeSerial &&
           left.indexHigh == right.indexHigh && left.indexLow == right.indexLow;
}

HMODULE FindLoadedByIdentity(const FileIdentity& wanted) {
    if (!wanted.valid) return nullptr;

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HMODULE retained = nullptr;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            // ModuleFile handles paths longer than MAX_PATH
            const std::wstring loadedPath = path::ModuleFile(entry.hModule);
            if (!loadedPath.empty() && SameIdentity(wanted, IdentityOf(loadedPath))) {
                // Keep our own reference even if a dependency loaded it first
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(entry.modBaseAddr), &retained);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return retained;
}

bool ReadExact(HANDLE file, void* output, DWORD size) {
    DWORD read = 0;
    return ReadFile(file, output, size, &read, nullptr) != FALSE && read == size;
}

bool IsX86Dll(const std::wstring& filePath) {
    HANDLE file = CreateFileW(
        filePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    IMAGE_DOS_HEADER dos{};
    bool valid = GetFileSizeEx(file, &fileSize) != FALSE &&
                 ReadExact(file, &dos, sizeof(dos)) &&
                 dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0;
    const LONGLONG minimumNtSize = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                                   sizeof(WORD);
    valid = valid && static_cast<LONGLONG>(dos.e_lfanew) <=
                         fileSize.QuadPart - minimumNtSize;
    if (valid) {
        LARGE_INTEGER ntOffset{};
        ntOffset.QuadPart = dos.e_lfanew;
        DWORD signature = 0;
        IMAGE_FILE_HEADER header{};
        WORD optionalMagic = 0;
        valid = SetFilePointerEx(file, ntOffset, nullptr, FILE_BEGIN) != FALSE &&
                ReadExact(file, &signature, sizeof(signature)) &&
                ReadExact(file, &header, sizeof(header)) &&
                ReadExact(file, &optionalMagic, sizeof(optionalMagic)) &&
                signature == IMAGE_NT_SIGNATURE &&
                header.Machine == IMAGE_FILE_MACHINE_I386 &&
                (header.Characteristics & IMAGE_FILE_DLL) != 0 &&
                header.SizeOfOptionalHeader >= sizeof(optionalMagic) &&
                optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    }
    CloseHandle(file);
    return valid;
}

void Record(Status status) {
    switch (status) {
    case Status::Loaded:
    case Status::AlreadyLoaded:
        ++g_report.loaded;
        break;
    case Status::SkippedDuplicate:
    case Status::SkippedReserved:
    case Status::SkippedCompetingFix:
        ++g_report.skipped;
        break;
    case Status::Failed:
        ++g_report.failed;
        break;
    }
}

HMODULE Load(const std::wstring& filePath, DWORD* error) {
    if (error) *error = ERROR_SUCCESS;
    constexpr DWORD flags =
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    HMODULE module = LoadLibraryExW(filePath.c_str(), nullptr, flags);
    if (module) return module;

    const DWORD safeError = GetLastError();
    // Older Wine may not support LOAD_LIBRARY_SEARCH_*
    // Fall back to ALTERED_SEARCH_PATH so dependencies still resolve beside the add-on
    if (safeError == ERROR_INVALID_PARAMETER ||
        safeError == ERROR_CALL_NOT_IMPLEMENTED) {
        module = LoadLibraryExW(
            filePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module) return module;
    }
    const DWORD loadError = GetLastError();
    if (error) *error = loadError ? loadError : safeError;
    return nullptr;
}

BOOL CALLBACK LoadAll(PINIT_ONCE, PVOID, PVOID*) {
    const HMODULE ownModule = OwnModule();
    const std::wstring ownPath = path::ModuleFile(ownModule);
    const std::wstring directory = path::Directory(ownPath);
    if (directory.empty()) {
        LogWarning("Add-on loader disabled: NovaFix directory unavailable");
        return TRUE;
    }

    std::array<bool, compat::kAddonDirectoryNames.size()>
        existingDirectories{};
    for (size_t index = 0; index < compat::kAddonDirectoryNames.size();
         ++index) {
        const std::wstring candidate = Join(
            directory, compat::kAddonDirectoryNames[index]);
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        existingDirectories[index] =
            attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    const auto selection = compat::ResolveAddonDirectorySelection(
        g_reuseExistingDirectory.load(std::memory_order_acquire),
        existingDirectories);
    g_report.directory = Join(
        directory, compat::kAddonDirectoryNames[selection.primaryIndex]);
    if (selection.createPrimary &&
        !CreateDirectoryW(g_report.directory.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            LogWarning("Add-on directory creation failed: path=%ls error=%lu",
                g_report.directory.c_str(), error);
            return TRUE;
        }
    }

    std::vector<Candidate> candidates;
    for (size_t index = 0; index < compat::kAddonDirectoryNames.size(); ++index) {
        const std::wstring_view name = compat::kAddonDirectoryNames[index];
        const std::wstring candidateDirectory = Join(directory, name);
        const DWORD attributes = GetFileAttributesW(candidateDirectory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        Collect(candidateDirectory, L"", name, index, &candidates);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  const int insensitive = CompareStringOrdinal(
                      left.orderPath.c_str(), -1,
                      right.orderPath.c_str(), -1, TRUE);
                  if (insensitive != CSTR_EQUAL) return insensitive == CSTR_LESS_THAN;
                  if (left.directoryIndex != right.directoryIndex) {
                      return left.directoryIndex < right.directoryIndex;
                  }
                  return CompareStringOrdinal(
                             left.orderPath.c_str(), -1,
                             right.orderPath.c_str(), -1, FALSE) == CSTR_LESS_THAN;
              });

    const FileIdentity ownIdentity = IdentityOf(ownPath);
    std::vector<FileIdentity> seen;
    seen.reserve(candidates.size());

    for (const Candidate& candidate : candidates) {
        if (IsReservedRenderer(candidate.relativePath)) {
            Record(Status::SkippedReserved);
            Log("Add-on skipped: renderer belongs in D3D9 chain path=%ls",
                candidate.relativePath.c_str());
            continue;
        }

        const FileIdentity identity = IdentityOf(candidate.path);
        if (SameIdentity(identity, ownIdentity) ||
            std::any_of(seen.begin(), seen.end(), [&](const FileIdentity& item) {
                return SameIdentity(identity, item);
            })) {
            Record(Status::SkippedDuplicate);
            Log("Add-on skipped: duplicate or self path=%ls",
                candidate.relativePath.c_str());
            continue;
        }
        if (identity.valid) seen.push_back(identity);

        if (compatibility::ShouldBlockExtraLibrary(candidate.path)) {
            Record(Status::SkippedCompetingFix);
            continue;
        }

        if (HMODULE existing = FindLoadedByIdentity(identity)) {
            addon_storage::Register(existing, candidate.path);
            plugin_controls::Discover(existing, candidate.relativePath);
            Record(Status::AlreadyLoaded);
            Log("Add-on already loaded: path=%ls",
                candidate.relativePath.c_str());
            continue;
        }

        if (!IsX86Dll(candidate.path)) {
            Record(Status::Failed);
            LogWarning("Add-on rejected: invalid or non-x86 path=%ls",
                candidate.relativePath.c_str());
            continue;
        }

        DWORD error = ERROR_SUCCESS;
        HMODULE module = Load(candidate.path, &error);
        if (!module) {
            Record(Status::Failed);
            LogWarning("Add-on load failed: path=%ls error=%lu",
                candidate.relativePath.c_str(), error);
            continue;
        }

        addon_storage::Register(module, candidate.path);
        plugin_controls::Discover(module, candidate.relativePath);
        Record(Status::Loaded);
        if (GetProcAddress(module, "Direct3DCreate9")) {
            LogWarning(
                "D3D9 renderer loaded as add-on: path=%ls; use d3d9_chain.dll, " 
                "dxvk.dll, or ReShade\\d3d9.dll as the renderer backend",
                candidate.relativePath.c_str());
        } else {
            LogInfo("Add-on loaded: path=%ls", candidate.relativePath.c_str());
        }
    }

    Log("Add-on scan: directory=%ls loaded=%u skipped=%u failed=%u",
        g_report.directory.c_str(), g_report.loaded, g_report.skipped, g_report.failed);
    return TRUE;
}

} // namespace

void SetReuseExistingDirectoryPolicy(bool enabled) {
    g_reuseExistingDirectory.store(enabled, std::memory_order_release);
}

void Initialize() {
    InitOnceExecuteOnce(&g_loadOnce, &LoadAll, nullptr, nullptr);
}

unsigned LoadedCount() {
    Initialize();
    return g_report.loaded;
}

} // namespace novafix::compat::addon_loader
