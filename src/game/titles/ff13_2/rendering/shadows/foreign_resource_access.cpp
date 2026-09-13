#include "game/titles/ff13_2/rendering/shadows/foreign_resource_access.h"

#include <windows.h>

#include <cstdint>

namespace novafix::game::xiii2_shadow_foreign_resource {

bool IsReadable(const void* address, std::size_t size) {
    if (!address || size == 0u) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & readable) == 0u) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionEnd =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return begin <= regionEnd && size <= regionEnd - begin;
}

bool IsWritable(const void* address, std::size_t size) {
    if (!address || size == 0u) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & writable) == 0u) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionEnd =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return begin <= regionEnd && size <= regionEnd - begin;
}

bool IsExecutable(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (memory.Protect & executable) != 0u;
}

bool ValidateComObject(void* object) {
    void** vtable{};
    return ReadField(object, 0, vtable) && vtable &&
           IsReadable(vtable, sizeof(void*) * 3u) &&
           IsExecutable(vtable[0]) && IsExecutable(vtable[1]) &&
           IsExecutable(vtable[2]);
}

bool ResourceBelongsToDevice(IDirect3DResource9* resource,
                             IDirect3DDevice9* expected) {
    if (!resource || !expected) return false;
    IDirect3DDevice9* owner{};
    const HRESULT result = resource->GetDevice(&owner);
    IUnknown* ownerIdentity{};
    IUnknown* expectedIdentity{};
    const bool matches =
        SUCCEEDED(result) && owner &&
        SUCCEEDED(owner->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&ownerIdentity))) &&
        SUCCEEDED(expected->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&expectedIdentity))) &&
        ownerIdentity == expectedIdentity;
    if (expectedIdentity) expectedIdentity->Release();
    if (ownerIdentity) ownerIdentity->Release();
    if (owner) owner->Release();
    return matches;
}

} // namespace novafix::game::xiii2_shadow_foreign_resource
