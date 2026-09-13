#include "patch/iat_hook.h"

#include "patch/pointer_hook.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace novafix {
namespace {

bool RangeInImage(DWORD rva, std::size_t size, DWORD imageSize) {
    return rva <= imageSize && size <= static_cast<std::size_t>(imageSize - rva);
}

const char* ImageString(unsigned char* base, DWORD imageSize, DWORD rva) {
    if (!RangeInImage(rva, 1u, imageSize)) return nullptr;
    auto* text = reinterpret_cast<const char*>(base + rva);
    const std::size_t available = imageSize - rva;
    return std::memchr(text, '\0', available) ? text : nullptr;
}

bool ModuleLayout(HMODULE module, unsigned char** baseOut,
                  DWORD* imageSizeOut,
                  IMAGE_DATA_DIRECTORY* importsOut) {
    if (!module || !baseOut || !imageSizeOut || !importsOut) return false;
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x100000) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    *baseOut = base;
    *imageSizeOut = nt->OptionalHeader.SizeOfImage;
    *importsOut = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    return true;
}

void** FindModuleImportSlot(HMODULE module, const char* dllName,
                            const char* functionName, WORD ordinal,
                            bool matchOrdinal) {
    if (!dllName || (!matchOrdinal && !functionName)) return nullptr;
    unsigned char* base{};
    DWORD imageSize{};
    IMAGE_DATA_DIRECTORY directory{};
    if (!ModuleLayout(module, &base, &imageSize, &directory) ||
        !directory.VirtualAddress ||
        !RangeInImage(directory.VirtualAddress,
                      sizeof(IMAGE_IMPORT_DESCRIPTOR), imageSize)) {
        return nullptr;
    }
    const std::size_t availableDescriptors =
        (std::min<std::size_t>)(
            directory.Size ? directory.Size : imageSize - directory.VirtualAddress,
            imageSize - directory.VirtualAddress) /
        sizeof(IMAGE_IMPORT_DESCRIPTOR);
    auto* descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < availableDescriptors; ++descriptorIndex) {
        IMAGE_IMPORT_DESCRIPTOR& descriptor = descriptors[descriptorIndex];
        if (!descriptor.Name) break;
        const char* importedDll = ImageString(base, imageSize, descriptor.Name);
        if (!importedDll) return nullptr;
        if (_stricmp(importedDll, dllName) != 0) continue;
        if (!descriptor.OriginalFirstThunk || !descriptor.FirstThunk ||
            !RangeInImage(descriptor.OriginalFirstThunk,
                          sizeof(IMAGE_THUNK_DATA), imageSize) ||
            !RangeInImage(descriptor.FirstThunk,
                          sizeof(IMAGE_THUNK_DATA), imageSize)) {
            return nullptr;
        }

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor.OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor.FirstThunk);
        const std::size_t thunkCount = (std::min)(
            (imageSize - descriptor.OriginalFirstThunk) / sizeof(IMAGE_THUNK_DATA),
            (imageSize - descriptor.FirstThunk) / sizeof(IMAGE_THUNK_DATA));
        for (std::size_t thunkIndex = 0; thunkIndex < thunkCount; ++thunkIndex) {
            if (!names[thunkIndex].u1.AddressOfData) break;
            const bool importedByOrdinal =
                IMAGE_SNAP_BY_ORDINAL(names[thunkIndex].u1.Ordinal) != FALSE;
            if (matchOrdinal) {
                if (!importedByOrdinal ||
                    IMAGE_ORDINAL(names[thunkIndex].u1.Ordinal) != ordinal) {
                    continue;
                }
            } else {
                if (importedByOrdinal) continue;
                const DWORD nameRva = static_cast<DWORD>(
                    names[thunkIndex].u1.AddressOfData);
                const DWORD kNameOffset =
                    static_cast<DWORD>(offsetof(IMAGE_IMPORT_BY_NAME, Name));
                if (!RangeInImage(nameRva, kNameOffset + 1u, imageSize)) return nullptr;
                const char* importedName = ImageString(
                    base, imageSize, nameRva + kNameOffset);
                if (!importedName) return nullptr;
                if (std::strcmp(importedName, functionName) != 0) continue;
            }
            return reinterpret_cast<void**>(&slots[thunkIndex].u1.Function);
        }
    }
    return nullptr;
}

bool PatchModuleImportImpl(HMODULE module, const char* ownerName,
                           const char* dllName, const char* functionName,
                           WORD ordinal, bool matchOrdinal, void* replacement,
                           void** original) {
    if (!ownerName || !replacement || !original) return false;
    *original = nullptr;
    void** const slot = FindModuleImportSlot(
        module, dllName, functionName, ordinal, matchOrdinal);
    if (!slot) return false;

    const std::string symbol = matchOrdinal
        ? "#" + std::to_string(ordinal)
        : functionName;
    const std::string feature = std::string("iat:") + ownerName + ":" +
        dllName + "!" + symbol;
    return patch::InstallPointerHook(
        feature, slot, replacement, original,
        patch::registry::Kind::ImportHook);
}

} // namespace

bool PatchModuleImport(HMODULE module, const char* ownerName, const char* dllName,
                       const char* functionName, void* replacement, void** original) {
    return PatchModuleImportImpl(module, ownerName, dllName, functionName, 0,
                                 false, replacement, original);
}

bool PatchModuleImportOrdinal(HMODULE module, const char* ownerName,
                              const char* dllName, WORD ordinal,
                              void* replacement, void** original) {
    return PatchModuleImportImpl(module, ownerName, dllName, nullptr, ordinal,
                                 true, replacement, original);
}

bool PatchExecutableImport(const char* dllName, const char* functionName,
                           void* replacement, void** original) {
    return PatchModuleImport(GetModuleHandleW(nullptr), "executable", dllName,
                             functionName, replacement, original);
}

bool PatchExecutableImportOrdinal(const char* dllName, WORD ordinal,
                                  void* replacement, void** original) {
    return PatchModuleImportOrdinal(GetModuleHandleW(nullptr), "executable",
                                    dllName, ordinal, replacement, original);
}

void* ReadExecutableImportTarget(const char* dllName,
                                 const char* functionName) {
    void** const slot = FindModuleImportSlot(
        GetModuleHandleW(nullptr), dllName, functionName, 0u, false);
    return slot ? *slot : nullptr;
}

} // namespace novafix
