#pragma once

#include <d3d9.h>

#include <cstddef>
#include <cstring>

namespace novafix::game::xiii2_shadow_foreign_resource {

bool IsReadable(const void* address, std::size_t size);
bool IsWritable(const void* address, std::size_t size);
bool IsExecutable(const void* address);

template <typename T>
bool ReadField(const void* object, std::ptrdiff_t offset, T& value) {
    if (!object) return false;
    const auto* address = static_cast<const std::byte*>(object) + offset;
    if (!IsReadable(address, sizeof(T))) return false;
    std::memcpy(&value, address, sizeof(T));
    return true;
}

template <typename T>
bool FieldWritable(void* object, std::ptrdiff_t offset) {
    return object &&
           IsWritable(static_cast<std::byte*>(object) + offset, sizeof(T));
}

template <typename T>
void WriteField(void* object, std::ptrdiff_t offset, const T& value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(T));
}

bool ValidateComObject(void* object);
bool ResourceBelongsToDevice(IDirect3DResource9* resource,
                             IDirect3DDevice9* expected);

} // namespace novafix::game::xiii2_shadow_foreign_resource
