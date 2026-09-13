#include "compat/addons/plugin_descriptor.h"

#include "common/guarded_call.h"
#include "diagnostics/log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace novafix::compat::plugin_descriptor {
namespace {

constexpr std::size_t kMaxIdLength = 64u;
constexpr std::size_t kMaxLabelLength = 128u;
constexpr std::size_t kMaxDescriptionLength = 1024u;
constexpr std::uint32_t kMaxControls = 256u;
constexpr std::uint32_t kMaxComboOptions = 256u;
constexpr std::uint32_t kMaxNativeMenus = 32u;
constexpr std::uint32_t kMaxNativeMenuItems = 512u;
constexpr float kSafeIntegerLimit = 16000000.0f;
constexpr std::size_t kDescriptorRequiredSize =
    offsetof(NovaFixPluginDescriptor, nativeMenuCount);
constexpr std::size_t kControlRequiredSize =
    offsetof(NovaFixPluginControl, activate);
constexpr std::size_t kDescriptorControlStrideSize =
    offsetof(NovaFixPluginDescriptor, controlStride) +
    sizeof(std::uint32_t);

bool TryCopyNativeMenu(const void* source,
                       NovaFixPluginNativeMenu* destination,
                       DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        const auto* menu =
            static_cast<const NovaFixPluginNativeMenu*>(source);
        if (menu->structSize < sizeof(NovaFixPluginNativeMenu)) return false;
        *destination = *menu;
        return true;
    }, false, exceptionCode);
}

bool TryCopyNativeMenuItem(const void* source,
                           NovaFixPluginNativeMenuItem* destination,
                           DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        const auto* item =
            static_cast<const NovaFixPluginNativeMenuItem*>(source);
        if (item->structSize < sizeof(NovaFixPluginNativeMenuItem)) {
            return false;
        }
        *destination = *item;
        return true;
    }, false, exceptionCode);
}

bool TryCopyControl(const void* source,
                    NovaFixPluginControl* destination,
                    DWORD* exceptionCode) noexcept {
    if (!source || !destination || !exceptionCode) return false;
    return guarded_call::ResultOr<bool>([&] {
        const auto* control =
            static_cast<const NovaFixPluginControl*>(source);
        if (control->structSize < kControlRequiredSize) return false;
        std::memset(destination, 0, sizeof(*destination));
        std::memcpy(destination, control,
                    std::min<std::size_t>(control->structSize,
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
              bool* truncated = nullptr) {
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

std::string SourceName(std::wstring_view source) {
    const std::size_t separator = source.find_last_of(L"\\/");
    if (separator != std::wstring_view::npos) {
        source.remove_prefix(separator + 1u);
    }
    if (source.empty()) return "Unnamed addon";

    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
        static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "Unnamed addon";
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
            static_cast<int>(source.size()), result.data(), required,
            nullptr, nullptr) != required) {
        return "Unnamed addon";
    }
    return result;
}

bool IsKnownType(std::uint32_t type) {
    return type >= NOVAFIX_PLUGIN_CONTROL_CHECKBOX &&
           type <= NOVAFIX_PLUGIN_CONTROL_TEXT;
}

bool IsKnownNativeMenuType(std::uint32_t type) {
    return type >= NOVAFIX_PLUGIN_NATIVE_MENU_ACTION &&
           type <= NOVAFIX_PLUGIN_NATIVE_MENU_SUBMENU;
}

void Disable(plugin_controls::Control* control, std::string_view issue) {
    if (!control) return;
    control->available = false;
    control->issue.assign(issue);
}

bool CopyComboOptions(const NovaFixPluginControl& source,
                      plugin_controls::Control* output,
                      std::string* warning) {
    if (!output) return false;
    if (!source.optionCount || !source.options) {
        Disable(output, "Combo has no options");
        return true;
    }
    if (source.optionCount > kMaxComboOptions) {
        Disable(output, "Combo has too many options");
        return true;
    }

    output->options.reserve(source.optionCount);
    for (std::uint32_t optionIndex = 0u;
         optionIndex < source.optionCount; ++optionIndex) {
        const char* optionAddress = nullptr;
        DWORD exceptionCode = 0;
        if (!TryCopyTextPointer(source.options, optionIndex, &optionAddress,
                                &exceptionCode)) {
            Disable(output, "Combo option list is unreadable");
            output->options.clear();
            return true;
        }
        const std::string fallback =
            "Option " + std::to_string(optionIndex + 1u);
        std::string option;
        bool truncated = false;
        if (!CopyText(optionAddress, kMaxLabelLength, fallback, &option,
                      &truncated) || truncated) {
            if (warning && warning->empty()) {
                *warning = "combo option text was replaced or truncated";
            }
        }
        output->options.push_back(std::move(option));
    }
    output->minimum = 0.0f;
    output->maximum = static_cast<float>(output->options.size() - 1u);
    output->step = 1.0f;
    return true;
}

bool CopyControl(const void* address, std::uint32_t index,
                 plugin_controls::Control* output,
                 std::string* warning) {
    if (!address || !output) return false;
    NovaFixPluginControl source{};
    DWORD exceptionCode = 0;
    if (!TryCopyControl(address, &source, &exceptionCode)) {
        if (warning) *warning = "descriptor memory is unreadable";
        return false;
    }
    const std::string fallbackId =
        "control_" + std::to_string(index + 1u);
    bool idTruncated = false;
    bool labelTruncated = false;
    const bool idReadable = CopyText(
        source.id, kMaxIdLength, fallbackId, &output->id, &idTruncated);
    const std::string_view fallbackLabel =
        source.type == NOVAFIX_PLUGIN_CONTROL_SEPARATOR
            ? std::string_view{}
            : std::string_view(output->id);
    const bool labelReadable = CopyText(
        source.label, kMaxLabelLength, fallbackLabel, &output->label,
        &labelTruncated);
    const bool descriptionReadable = CopyText(
        source.description, kMaxDescriptionLength, {},
        &output->description);
    output->type = source.type;
    output->readValue = source.readValue;
    output->writeValue = source.writeValue;
    output->activate = source.activate;
    output->userData = source.userData;

    if (!idReadable || !labelReadable || !descriptionReadable) {
        if (warning) {
            *warning = "unreadable text was replaced with a fallback";
        }
    } else if (idTruncated || labelTruncated) {
        if (warning) *warning = "overlong text was truncated";
    }

    if (!IsKnownType(source.type)) {
        Disable(output, "Unsupported control type");
        return true;
    }

    if (source.type == NOVAFIX_PLUGIN_CONTROL_HEADER ||
        source.type == NOVAFIX_PLUGIN_CONTROL_SEPARATOR ||
        source.type == NOVAFIX_PLUGIN_CONTROL_TEXT) {
        return true;
    }
    if (source.type == NOVAFIX_PLUGIN_CONTROL_BUTTON) {
        if (!source.activate) Disable(output, "Missing activate callback");
        return true;
    }
    if (!source.readValue || !source.writeValue) {
        Disable(output, "Missing read or write callback");
        return true;
    }

    if (source.type == NOVAFIX_PLUGIN_CONTROL_CHECKBOX) {
        output->minimum = 0.0f;
        output->maximum = 1.0f;
        output->step = 1.0f;
        return true;
    }
    if (source.type == NOVAFIX_PLUGIN_CONTROL_COMBO) {
        return CopyComboOptions(source, output, warning);
    }

    if (!std::isfinite(source.minimum) ||
        !std::isfinite(source.maximum) ||
        !std::isfinite(source.step) ||
        source.minimum >= source.maximum || source.step <= 0.0f) {
        Disable(output, "Invalid slider range");
        return true;
    }
    if (source.type == NOVAFIX_PLUGIN_CONTROL_INT_SLIDER &&
        (source.minimum < -kSafeIntegerLimit ||
         source.maximum > kSafeIntegerLimit || source.step < 1.0f ||
         source.minimum != std::round(source.minimum) ||
         source.maximum != std::round(source.maximum) ||
         source.step != std::round(source.step))) {
        Disable(output, "Integer slider values must be whole numbers");
        return true;
    }
    output->minimum = source.minimum;
    output->maximum = source.maximum;
    output->step = source.step;
    return true;
}

bool CopyNativeMenuItem(const void* address, std::uint32_t index,
                        plugin_controls::NativeMenuItem* output,
                        std::string* warning) {
    if (!address || !output) return false;
    NovaFixPluginNativeMenuItem source{};
    DWORD exceptionCode = 0;
    if (!TryCopyNativeMenuItem(address, &source, &exceptionCode)) {
        if (warning) *warning = "item memory is unreadable";
        return false;
    }

    const std::string fallbackId =
        "item_" + std::to_string(index + 1u);
    bool idTruncated = false;
    bool labelTruncated = false;
    const bool idReadable = CopyText(
        source.id, kMaxIdLength, fallbackId, &output->id, &idTruncated);
    const bool labelReadable = CopyText(
        source.label, kMaxLabelLength, output->id, &output->label,
        &labelTruncated);
    output->type = source.type;
    output->parentIndex = source.parentIndex;
    output->readState = source.readState;
    output->activate = source.activate;
    output->userData = source.userData;

    if (!idReadable || !labelReadable) {
        if (warning) {
            *warning = "unreadable text was replaced with a fallback";
        }
    } else if (idTruncated || labelTruncated) {
        if (warning) *warning = "overlong text was truncated";
    }
    if (!IsKnownNativeMenuType(source.type)) {
        output->available = false;
        output->issue = "Unsupported native menu item type";
        return true;
    }
    if (source.type != NOVAFIX_PLUGIN_NATIVE_MENU_SEPARATOR &&
        source.type != NOVAFIX_PLUGIN_NATIVE_MENU_SUBMENU &&
        !source.activate) {
        output->available = false;
        output->issue = "Missing activate callback";
    }
    return true;
}

bool CopyNativeMenu(const void* address, std::uint32_t index,
                    plugin_controls::NativeMenu* output,
                    std::string* warning) {
    if (!address || !output) return false;
    NovaFixPluginNativeMenu source{};
    DWORD exceptionCode = 0;
    if (!TryCopyNativeMenu(address, &source, &exceptionCode)) {
        if (warning) *warning = "menu memory is unreadable";
        return false;
    }
    const std::string fallbackId =
        "menu_" + std::to_string(index + 1u);
    CopyText(source.id, kMaxIdLength, fallbackId, &output->id);
    CopyText(source.label, kMaxLabelLength, output->id, &output->label);

    if (!source.itemCount) return true;
    if (!source.items ||
        source.itemStride < sizeof(NovaFixPluginNativeMenuItem)) {
        if (warning) *warning = "item array is unavailable";
        return true;
    }
    const std::uint32_t itemCount =
        std::min(source.itemCount, kMaxNativeMenuItems);
    output->items.reserve(itemCount);
    for (std::uint32_t itemIndex = 0u; itemIndex < itemCount;
         ++itemIndex) {
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(source.items);
        const std::uintptr_t offset =
            static_cast<std::uintptr_t>(itemIndex) *
            static_cast<std::uintptr_t>(source.itemStride);
        if (base > UINTPTR_MAX - offset) break;
        plugin_controls::NativeMenuItem item{};
        std::string itemWarning;
        if (!CopyNativeMenuItem(
                reinterpret_cast<const void*>(base + offset), itemIndex,
                &item, &itemWarning)) {
            LogWarning("Add-on native menu item unavailable: menu=%s index=%u reason=%s",
                output->id.c_str(), itemIndex,
                itemWarning.empty() ? "unreadable descriptor"
                                    : itemWarning.c_str());
            item.type = NOVAFIX_PLUGIN_NATIVE_MENU_ACTION;
            item.id = "unavailable_" + std::to_string(itemIndex + 1u);
            item.label = "Unavailable item";
            item.available = false;
            item.issue = "Unreadable descriptor";
        }
        if (item.parentIndex >= static_cast<std::int32_t>(itemIndex) ||
            (item.parentIndex >= 0 &&
             output->items[static_cast<std::size_t>(item.parentIndex)].type !=
                 NOVAFIX_PLUGIN_NATIVE_MENU_SUBMENU)) {
            LogWarning("Add-on native menu item moved to root: menu=%s item=%s reason=invalid parent",
                output->id.c_str(), item.id.c_str());
            item.parentIndex = -1;
        }
        if (!itemWarning.empty()) {
            LogWarning("Add-on native menu item fallback: menu=%s item=%s reason=%s",
                output->id.c_str(), item.id.c_str(), itemWarning.c_str());
        }
        output->items.push_back(std::move(item));
    }
    if (source.itemCount > kMaxNativeMenuItems) {
        LogWarning("Add-on native menu item limit: menu=%s requested=%u used=%u",
            output->id.c_str(), source.itemCount, itemCount);
    }
    return true;
}

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

void ReadIdentity(const NovaFixPluginDescriptor& descriptor,
                  std::wstring_view source, std::size_t fallbackIndex,
                  plugin_controls::Plugin& plugin) {
    const std::string fallbackName = SourceName(source);
    const std::string fallbackId =
        "addon_" + std::to_string(fallbackIndex + 1u);
    CopyText(descriptor.id, kMaxIdLength, fallbackId, &plugin.id);
    CopyText(descriptor.name, kMaxLabelLength, fallbackName, &plugin.name);
}

void ReadControls(const NovaFixPluginDescriptor& descriptor,
                  plugin_controls::Plugin& plugin) {
    const std::uint32_t controlCount =
        std::min(descriptor.controlCount, kMaxControls);
    if (descriptor.controlCount > kMaxControls) {
        LogWarning("Add-on control limit: source=%ls id=%s requested=%u used=%u",
            plugin.source.c_str(), plugin.id.c_str(),
            descriptor.controlCount, controlCount);
    }
    if (controlCount && !descriptor.controls) {
        LogWarning("Add-on controls unavailable: source=%ls id=%s reason=missing array",
            plugin.source.c_str(), plugin.id.c_str());
        return;
    }

    std::uint32_t controlStride =
        static_cast<std::uint32_t>(kControlRequiredSize);
    if (descriptor.structSize >= kDescriptorControlStrideSize) {
        controlStride = descriptor.controlStride
                            ? descriptor.controlStride
                            : static_cast<std::uint32_t>(
                                  sizeof(NovaFixPluginControl));
        if (controlStride < kControlRequiredSize) {
            LogWarning("Add-on controls unavailable: source=%ls id=%s reason=invalid stride",
                       plugin.source.c_str(), plugin.id.c_str());
            return;
        }
    }

    plugin.controls.reserve(controlCount);
    for (std::uint32_t index = 0u; index < controlCount; ++index) {
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(descriptor.controls);
        const std::uintptr_t offset =
            static_cast<std::uintptr_t>(index) *
            static_cast<std::uintptr_t>(controlStride);
        if (base > UINTPTR_MAX - offset) break;
        plugin_controls::Control control{};
        std::string warning;
        if (!CopyControl(reinterpret_cast<const void*>(base + offset),
                         index, &control, &warning)) {
            LogWarning("Add-on control skipped: source=%ls id=%s index=%u reason=%s",
                plugin.source.c_str(), plugin.id.c_str(), index,
                warning.empty() ? "unreadable descriptor"
                                : warning.c_str());
            continue;
        }
        if (!warning.empty()) {
            LogWarning("Add-on control fallback: source=%ls id=%s control=%s reason=%s",
                plugin.source.c_str(), plugin.id.c_str(),
                control.id.c_str(), warning.c_str());
        }
        plugin.controls.push_back(std::move(control));
    }
}

void ReadNativeMenus(const NovaFixPluginDescriptor& descriptor,
                     plugin_controls::Plugin& plugin) {
    if (descriptor.nativeMenuCount && descriptor.nativeMenus &&
        descriptor.nativeMenuStride >= sizeof(NovaFixPluginNativeMenu)) {
        const std::uint32_t menuCount =
            std::min(descriptor.nativeMenuCount, kMaxNativeMenus);
        plugin.nativeMenus.reserve(menuCount);
        for (std::uint32_t menuIndex = 0u; menuIndex < menuCount;
             ++menuIndex) {
            const std::uintptr_t base =
                reinterpret_cast<std::uintptr_t>(descriptor.nativeMenus);
            const std::uintptr_t offset =
                static_cast<std::uintptr_t>(menuIndex) *
                descriptor.nativeMenuStride;
            if (base > UINTPTR_MAX - offset) break;
            plugin_controls::NativeMenu menu{};
            std::string warning;
            if (!CopyNativeMenu(
                    reinterpret_cast<const void*>(base + offset),
                    menuIndex, &menu, &warning)) {
                LogWarning("Add-on native menu skipped: source=%ls index=%u reason=%s",
                    plugin.source.c_str(), menuIndex,
                    warning.empty() ? "unreadable descriptor"
                                    : warning.c_str());
                continue;
            }
            if (!warning.empty()) {
                LogWarning("Add-on native menu degraded: source=%ls menu=%s reason=%s",
                    plugin.source.c_str(), menu.id.c_str(),
                    warning.c_str());
            }
            plugin.nativeMenus.push_back(std::move(menu));
        }
        if (descriptor.nativeMenuCount > kMaxNativeMenus) {
            LogWarning("Add-on native menu limit: source=%ls requested=%u used=%u",
                plugin.source.c_str(), descriptor.nativeMenuCount,
                menuCount);
        }
    } else if (descriptor.nativeMenuCount) {
        LogWarning("Add-on native menus unavailable: source=%ls reason=invalid array",
            plugin.source.c_str());
    }
}

bool SameId(const std::string& left, const std::string& right) {
    return _stricmp(left.c_str(), right.c_str()) == 0;
}

} // namespace novafix::compat::plugin_descriptor
