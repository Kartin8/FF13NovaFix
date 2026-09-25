#include "compat/addons/plugin_descriptor.h"
#include "compat/addons/plugin_descriptor_reader.h"

#include "diagnostics/log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace novafix::compat::plugin_descriptor {
namespace {

using reader::CopyText;
using reader::ElementAddress;
using reader::TryCopyNativeMenu;
using reader::TryCopyNativeMenuItem;
using reader::kMaxDescriptionLength;
using reader::kMaxIdLength;
using reader::kMaxLabelLength;
using reader::kNativeMenuItemRequiredSize;
using reader::kNativeMenuRequiredSize;

constexpr std::uint32_t kMaxNativeMenus =
    NOVAFIX_PLUGIN_MAX_NATIVE_MENUS;
constexpr std::uint32_t kMaxNativeMenuItems =
    NOVAFIX_PLUGIN_MAX_NATIVE_MENU_ITEMS;

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

bool IsKnownNativeMenuType(std::uint32_t type) {
    return type >= NOVAFIX_PLUGIN_NATIVE_MENU_ACTION &&
           type <= NOVAFIX_PLUGIN_NATIVE_MENU_SUBMENU;
}

bool IsNormalizedColor(const NovaFixPluginColor& color) {
    return std::isfinite(color.red) && std::isfinite(color.green) &&
           std::isfinite(color.blue) && std::isfinite(color.alpha) &&
           color.red >= 0.0f && color.red <= 1.0f &&
           color.green >= 0.0f && color.green <= 1.0f &&
           color.blue >= 0.0f && color.blue <= 1.0f &&
           color.alpha >= 0.0f && color.alpha <= 1.0f;
}

bool CopyNativeMenuItem(const void* address, std::uint32_t stride,
                        std::uint32_t index,
                        plugin_controls::NativeMenuItem* output,
                        std::string* warning) {
    if (!address || !output) return false;
    NovaFixPluginNativeMenuItem source{};
    DWORD exceptionCode = 0;
    if (!TryCopyNativeMenuItem(address, stride, &source, &exceptionCode)) {
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

bool CopyNativeMenu(const void* address, std::uint32_t stride,
                    std::uint32_t index,
                    plugin_controls::NativeMenu* output,
                    std::string* warning) {
    if (!address || !output) return false;
    NovaFixPluginNativeMenu source{};
    DWORD exceptionCode = 0;
    if (!TryCopyNativeMenu(address, stride, &source, &exceptionCode)) {
        if (warning) *warning = "menu memory is unreadable";
        return false;
    }
    const std::string fallbackId =
        "menu_" + std::to_string(index + 1u);
    CopyText(source.id, kMaxIdLength, fallbackId, &output->id);
    CopyText(source.label, kMaxLabelLength, output->id, &output->label);

    if (!source.itemCount) return true;
    if (!source.items || source.itemStride < kNativeMenuItemRequiredSize) {
        if (warning) *warning = "item array is unavailable";
        return true;
    }
    const std::uint32_t itemCount =
        std::min(source.itemCount, kMaxNativeMenuItems);
    output->items.reserve(itemCount);
    for (std::uint32_t itemIndex = 0u; itemIndex < itemCount;
         ++itemIndex) {
        const void* itemAddress = ElementAddress(
            source.items, itemIndex, source.itemStride);
        if (!itemAddress) break;
        plugin_controls::NativeMenuItem item{};
        std::string itemWarning;
        if (!CopyNativeMenuItem(
                itemAddress, source.itemStride, itemIndex,
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
        if (std::any_of(
                output->items.begin(), output->items.end(),
                [&](const plugin_controls::NativeMenuItem& existing) {
                    return SameId(existing.id, item.id);
                })) {
            LogWarning("Add-on native menu item disabled: menu=%s item=%s reason=duplicate id",
                       output->id.c_str(), item.id.c_str());
            item.available = false;
            item.issue = "Duplicate item ID";
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

void ReadIdentity(const NovaFixPluginDescriptor& descriptor,
                  std::wstring_view source, std::size_t fallbackIndex,
                  plugin_controls::Plugin& plugin) {
    const std::string fallbackName = SourceName(source);
    const std::string fallbackId =
        "addon_" + std::to_string(fallbackIndex + 1u);
    bool idTruncated{};
    bool nameTruncated{};
    const bool idReadable = CopyText(
        descriptor.id, kMaxIdLength, fallbackId, &plugin.id, &idTruncated);
    const bool nameReadable = CopyText(
        descriptor.name, kMaxLabelLength, fallbackName, &plugin.name,
        &nameTruncated);
    const bool metadataReadable =
        CopyText(descriptor.author, kMaxLabelLength, {}, &plugin.author) &&
        CopyText(descriptor.version, kMaxLabelLength, {}, &plugin.version) &&
        CopyText(descriptor.description, kMaxDescriptionLength, {},
                 &plugin.description) &&
        CopyText(descriptor.website, kMaxDescriptionLength, {},
                 &plugin.website);
    const NovaFixPluginColor headerColor = descriptor.headerColor;
    const auto colorSpecified = [](const NovaFixPluginColor& color) {
        return color.red != 0.0f || color.green != 0.0f ||
               color.blue != 0.0f || color.alpha != 0.0f;
    };
    const bool colorRequested = colorSpecified(headerColor);
    if (colorRequested && IsNormalizedColor(headerColor)) {
        plugin.headerColor = headerColor;
        plugin.hasHeaderColor = true;
        plugin.headerColorAppliesToContent =
            (descriptor.headerColorFlags &
             NOVAFIX_PLUGIN_HEADER_COLOR_APPLY_TO_CONTENT) != 0u;
    } else if (colorRequested) {
        const std::wstring sourceText(source);
        LogWarning("Add-on header color ignored: source=%ls id=%s reason=invalid color",
                   sourceText.c_str(), plugin.id.c_str());
    }
    if (!idReadable || !nameReadable || !metadataReadable || idTruncated ||
        nameTruncated) {
        const std::wstring sourceText(source);
        LogWarning("Add-on identity sanitized: source=%ls id=%s",
                   sourceText.c_str(), plugin.id.c_str());
    }
}

void ReadNativeMenus(const NovaFixPluginDescriptor& descriptor,
                     plugin_controls::Plugin& plugin) {
    if (descriptor.nativeMenuCount && descriptor.nativeMenus &&
        descriptor.nativeMenuStride >= kNativeMenuRequiredSize) {
        const std::uint32_t menuCount =
            std::min(descriptor.nativeMenuCount, kMaxNativeMenus);
        plugin.nativeMenus.reserve(menuCount);
        for (std::uint32_t menuIndex = 0u; menuIndex < menuCount;
             ++menuIndex) {
            const void* menuAddress = ElementAddress(
                descriptor.nativeMenus, menuIndex,
                descriptor.nativeMenuStride);
            if (!menuAddress) break;
            plugin_controls::NativeMenu menu{};
            std::string warning;
            if (!CopyNativeMenu(menuAddress, descriptor.nativeMenuStride,
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
            if (std::any_of(
                    plugin.nativeMenus.begin(), plugin.nativeMenus.end(),
                    [&](const plugin_controls::NativeMenu& existing) {
                        return SameId(existing.id, menu.id);
                    })) {
                LogWarning("Add-on native menu skipped: source=%ls menu=%s reason=duplicate id",
                           plugin.source.c_str(), menu.id.c_str());
                continue;
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
