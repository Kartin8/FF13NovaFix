#pragma once

#include "compat/addons/plugin_descriptor.h"
#include "common/guarded_call.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace novafix::compat::plugin_descriptor::reader {

inline constexpr std::size_t kMaxIdLength = NOVAFIX_PLUGIN_MAX_ID_BYTES;
inline constexpr std::size_t kMaxLabelLength = NOVAFIX_PLUGIN_MAX_LABEL_BYTES;
inline constexpr std::size_t kMaxDescriptionLength =
    NOVAFIX_PLUGIN_MAX_DESCRIPTION_BYTES;
inline constexpr std::size_t kControlRequiredSize =
    offsetof(NovaFixPluginControl, userData) + sizeof(void*);
inline constexpr std::size_t kNativeMenuRequiredSize =
    offsetof(NovaFixPluginNativeMenu, items) +
    sizeof(const NovaFixPluginNativeMenuItem*);
inline constexpr std::size_t kNativeMenuItemRequiredSize =
    offsetof(NovaFixPluginNativeMenuItem, userData) + sizeof(void*);
inline constexpr std::size_t kExtensionHeaderV1Size =
    offsetof(NovaFixPluginExtension, next) +
    sizeof(const NovaFixPluginExtension*);

const void* ElementAddress(const void* base, std::uint32_t index,
                           std::uint32_t stride);
bool TryCopyNativeMenu(const void* source, std::size_t stride,
                       NovaFixPluginNativeMenu* destination,
                       DWORD* exceptionCode) noexcept;
bool TryCopyNativeMenuItem(const void* source, std::size_t stride,
                           NovaFixPluginNativeMenuItem* destination,
                           DWORD* exceptionCode) noexcept;
bool TryCopyControl(const void* source, std::size_t stride,
                    NovaFixPluginControl* destination,
                    DWORD* exceptionCode) noexcept;
bool TryCopyExtensionHeader(const NovaFixPluginExtension* source,
                            NovaFixPluginExtension* destination,
                            DWORD* exceptionCode) noexcept;
template <typename T>
bool TryCopyExtension(const NovaFixPluginExtension* source, T* destination,
                      std::size_t requiredSize,
                      DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        if (source->structSize < requiredSize) return false;
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, source,
                    std::min<std::size_t>(source->structSize,
                                          sizeof(*destination)));
        return true;
    }, false, exceptionCode);
}

bool TryCopyTextPointer(const char* const* source, std::uint32_t index,
                        const char** destination,
                        DWORD* exceptionCode) noexcept;
bool CopyText(const char* source, std::size_t maximum,
              std::string_view fallback, std::string* output,
              bool* truncated = nullptr);

} // namespace novafix::compat::plugin_descriptor::reader
