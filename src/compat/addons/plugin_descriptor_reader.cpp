#include "compat/addons/plugin_descriptor_reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace novafix::compat::plugin_descriptor {
namespace reader {

const void* ElementAddress(const void* base, std::uint32_t index,
                           std::uint32_t stride) {
    if (!base || !stride ||
        index > UINTPTR_MAX / static_cast<std::uintptr_t>(stride)) {
        return nullptr;
    }
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t offset =
        static_cast<std::uintptr_t>(index) * stride;
    if (address > UINTPTR_MAX - offset) return nullptr;
    return reinterpret_cast<const void*>(address + offset);
}

bool TryCopyNativeMenu(const void* source, std::size_t stride,
                       NovaFixPluginNativeMenu* destination,
                       DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        const auto* menu =
            static_cast<const NovaFixPluginNativeMenu*>(source);
        if (menu->structSize < kNativeMenuRequiredSize ||
            menu->structSize > stride) {
            return false;
        }
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, menu,
                    std::min<std::size_t>(menu->structSize,
                                          sizeof(*destination)));
        return true;
    }, false, exceptionCode);
}

bool TryCopyNativeMenuItem(const void* source, std::size_t stride,
                           NovaFixPluginNativeMenuItem* destination,
                           DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        const auto* item =
            static_cast<const NovaFixPluginNativeMenuItem*>(source);
        if (item->structSize < kNativeMenuItemRequiredSize ||
            item->structSize > stride) {
            return false;
        }
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, item,
                    std::min<std::size_t>(item->structSize,
                                          sizeof(*destination)));
        return true;
    }, false, exceptionCode);
}

bool TryCopyControl(const void* source, std::size_t stride,
                    NovaFixPluginControl* destination,
                    DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        const auto* control =
            static_cast<const NovaFixPluginControl*>(source);
        if (control->structSize < kControlRequiredSize ||
            control->structSize > stride) {
            return false;
        }
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, control,
                    std::min<std::size_t>(control->structSize,
                                          sizeof(*destination)));
        return true;
    }, false, exceptionCode);
}

bool TryCopyExtensionHeader(const NovaFixPluginExtension* source,
                            NovaFixPluginExtension* destination,
                            DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        if (source->structSize < kExtensionHeaderV1Size) return false;
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, source,
                    std::min<std::size_t>(source->structSize,
                                          sizeof(*destination)));
        return true;
    }, false, exceptionCode);
}

bool TryCopyTextPointer(const char* const* source, std::uint32_t index,
                        const char** destination,
                        DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::Run([&] {
        *destination = source[index];
    }, exceptionCode);
}

bool TryCopyText(const char* source, char* destination,
                 std::size_t capacity, std::size_t* length,
                 bool* truncated, DWORD* exceptionCode) noexcept {
    if (!source || !destination || capacity < 2u || !length ||
        !truncated || !exceptionCode) {
        return false;
    }
    return guarded_call::ResultOr<bool>([&] {
        for (std::size_t index = 0u; index + 1u < capacity; ++index) {
            destination[index] = source[index];
            if (source[index] == '\0') {
                *length = index;
                *truncated = false;
                return true;
            }
        }
        destination[capacity - 1u] = '\0';
        *length = capacity - 1u;
        *truncated = true;
        return true;
    }, false, exceptionCode);
}

bool CopyText(const char* source, std::size_t maximum,
              std::string_view fallback, std::string* output,
              bool* truncated) {
    if (!output) return false;
    if (!source) {
        output->assign(fallback);
        if (truncated) *truncated = false;
        return true;
    }

    std::vector<char> buffer(maximum + 1u);
    std::size_t length = 0u;
    bool wasTruncated = false;
    DWORD exceptionCode = 0;
    if (!TryCopyText(source, buffer.data(), buffer.size(), &length,
                     &wasTruncated, &exceptionCode)) {
        output->assign(fallback);
        if (truncated) *truncated = false;
        return false;
    }
    output->assign(buffer.data(), length);
    if (output->empty()) output->assign(fallback);
    if (truncated) *truncated = wasTruncated;
    return true;
}

} // namespace reader

namespace {
constexpr std::size_t kDescriptorRequiredSize =
    offsetof(NovaFixPluginDescriptor, nativeMenuCount);
} // namespace

bool CopyDescriptor(const NovaFixPluginDescriptor* source,
                    NovaFixPluginDescriptor* destination,
                    DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        const std::uint32_t sourceSize = source->structSize;
        if (sourceSize < kDescriptorRequiredSize) return false;
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, source,
                    std::min<std::size_t>(sourceSize,
                                          sizeof(*destination)));
        return true;
    }, false, exceptionCode);
}

} // namespace novafix::compat::plugin_descriptor
