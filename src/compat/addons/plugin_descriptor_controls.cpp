#include "compat/addons/plugin_descriptor.h"
#include "compat/addons/plugin_descriptor_reader.h"

#include "diagnostics/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <unordered_map>

namespace novafix::compat::plugin_descriptor {
namespace {

using reader::CopyText;
using reader::ElementAddress;
using reader::TryCopyControl;
using reader::TryCopyExtension;
using reader::TryCopyExtensionHeader;
using reader::TryCopyTextPointer;
using reader::kControlRequiredSize;
using reader::kMaxDescriptionLength;
using reader::kMaxIdLength;
using reader::kMaxLabelLength;

constexpr std::size_t kMaxValueAffixLength =
    NOVAFIX_PLUGIN_MAX_VALUE_AFFIX_BYTES;
constexpr std::uint32_t kMaxControls = NOVAFIX_PLUGIN_MAX_CONTROLS;
constexpr std::uint32_t kMaxComboOptions =
    NOVAFIX_PLUGIN_MAX_COMBO_OPTIONS;
constexpr std::uint32_t kMaxExtensions =
    NOVAFIX_PLUGIN_MAX_CONTROL_EXTENSIONS;
constexpr float kSafeIntegerLimit = 16000000.0f;
constexpr std::size_t kLegacyControlSize =
    offsetof(NovaFixPluginControl, extensions);
constexpr std::size_t kDescriptorControlStrideSize =
    offsetof(NovaFixPluginDescriptor, controlStride) +
    sizeof(std::uint32_t);
constexpr std::size_t kControlStateExtensionV1Size =
    offsetof(NovaFixPluginControlStateExtension, userData) + sizeof(void*);
constexpr std::size_t kStatusTextExtensionV1Size =
    offsetof(NovaFixPluginStatusTextExtension, refreshIntervalMs) +
    sizeof(std::uint32_t);
constexpr std::size_t kColorExtensionV1Size =
    offsetof(NovaFixPluginColorExtension, flags) + sizeof(std::uint32_t);
constexpr std::size_t kPresentationExtensionV1Size =
    offsetof(NovaFixPluginPresentationExtension, flags) +
    sizeof(std::uint32_t);
constexpr std::size_t kValueDisplayExtensionV1Size =
    offsetof(NovaFixPluginValueDisplayExtension, decimalPlaces) +
    sizeof(std::uint32_t);

bool IsKnownType(std::uint32_t type) {
    return type >= NOVAFIX_PLUGIN_CONTROL_CHECKBOX &&
           type <= NOVAFIX_PLUGIN_CONTROL_RADIO_GROUP;
}

void Disable(plugin_controls::Control* control, std::string_view issue) {
    if (!control) return;
    control->available = false;
    control->issue.assign(issue);
}

void DisableExtensionChain(plugin_controls::Control* control,
                           std::string_view issue) {
    Disable(control, issue);
    control->readState = nullptr;
    control->presentationFlags = 0u;
}

void SetWarning(std::string* warning, std::string_view value) {
    if (warning && warning->empty()) warning->assign(value);
}

struct ValueDisplay {
    std::string prefix;
    std::string suffix;
    std::uint32_t decimalPlaces{NOVAFIX_PLUGIN_DECIMALS_DEFAULT};
    bool present{};
};

std::string EscapeFormatLiteral(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 4u);
    for (const char character : value) {
        if (character == '%') output.push_back('%');
        output.push_back(character);
    }
    return output;
}

void BuildValueFormat(plugin_controls::Control* control,
                      const ValueDisplay& display, bool integer) {
    if (!control) return;
    if (!display.present) return;
    control->valueFormat = EscapeFormatLiteral(display.prefix);
    if (integer) {
        control->valueFormat.append("%d");
    } else {
        const std::uint32_t decimalPlaces =
            display.decimalPlaces == NOVAFIX_PLUGIN_DECIMALS_DEFAULT
                ? (control->step < 0.01f ? 3u : 2u)
                : display.decimalPlaces;
        control->valueFormat.append("%.");
        control->valueFormat.append(std::to_string(decimalPlaces));
        control->valueFormat.push_back('f');
    }
    control->valueFormat.append(EscapeFormatLiteral(display.suffix));
}

void ReadControlExtensions(const NovaFixPluginExtension* first,
                           plugin_controls::Control* output,
                           ValueDisplay* valueDisplay,
                           std::string* warning,
                           std::vector<const NovaFixPluginExtension*>& chain) {
    if (!output || !valueDisplay) return;
    std::array<const NovaFixPluginExtension*, kMaxExtensions> visited{};
    const NovaFixPluginExtension* current = first;
    std::uint32_t count = 0u;
    bool presentationSeen = false;
    bool valueDisplaySeen = false;
    while (current && count < kMaxExtensions) {
        if (std::find(visited.begin(), visited.begin() + count, current) !=
            visited.begin() + count) {
            SetWarning(warning, "cyclic extension chain was truncated");
            DisableExtensionChain(output,
                "Invalid extension chain: cycle detected. Attach each extension only once.");
            break;
        }
        visited[count++] = current;
        chain.push_back(current);

        NovaFixPluginExtension header{};
        DWORD exceptionCode{};
        if (!TryCopyExtensionHeader(current, &header, &exceptionCode)) {
            SetWarning(warning, "unreadable extension chain was truncated");
            DisableExtensionChain(output, "Invalid extension chain: unreadable extension.");
            break;
        }

        if (header.version >= NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION &&
            header.type == NOVAFIX_PLUGIN_EXTENSION_CONTROL_STATE) {
            NovaFixPluginControlStateExtension extension{};
            if (TryCopyExtension(current, &extension,
                                 kControlStateExtensionV1Size,
                                 &exceptionCode) &&
                extension.readState && !output->readState) {
                output->readState = extension.readState;
                output->stateUserData = extension.userData;
            } else if (extension.readState && output->readState) {
                SetWarning(warning,
                           "duplicate control state extension was ignored");
            } else if (!extension.readState) {
                SetWarning(warning, "control state extension is invalid");
            }
        } else if (header.version >=
                       NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION &&
                   header.type == NOVAFIX_PLUGIN_EXTENSION_STATUS_TEXT) {
            NovaFixPluginStatusTextExtension extension{};
            if (TryCopyExtension(current, &extension,
                                 kStatusTextExtensionV1Size,
                                 &exceptionCode) &&
                extension.readText && !output->readText) {
                output->readText = extension.readText;
                output->textUserData = extension.userData;
                output->refreshIntervalMs = extension.refreshIntervalMs;
            } else if (extension.readText && output->readText) {
                SetWarning(warning,
                           "duplicate status text extension was ignored");
            } else if (!extension.readText) {
                SetWarning(warning, "status text extension is invalid");
            }
        } else if (header.version >=
                       NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION &&
                   header.type == NOVAFIX_PLUGIN_EXTENSION_COLOR) {
            NovaFixPluginColorExtension extension{};
            if (TryCopyExtension(current, &extension,
                                 kColorExtensionV1Size,
                                 &exceptionCode) &&
                extension.readColor && extension.writeColor &&
                !output->readColor) {
                output->readColor = extension.readColor;
                output->writeColor = extension.writeColor;
                output->colorUserData = extension.userData;
                output->colorFlags =
                    extension.flags & NOVAFIX_PLUGIN_COLOR_ALPHA;
            } else if (extension.readColor && extension.writeColor &&
                       output->readColor) {
                SetWarning(warning,
                           "duplicate color extension was ignored");
            } else if (!extension.readColor || !extension.writeColor) {
                SetWarning(warning, "color extension is invalid");
            }
        } else if (header.version >=
                       NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION &&
                   header.type == NOVAFIX_PLUGIN_EXTENSION_PRESENTATION) {
            NovaFixPluginPresentationExtension extension{};
            if (!presentationSeen &&
                TryCopyExtension(current, &extension,
                                 kPresentationExtensionV1Size,
                                 &exceptionCode)) {
                constexpr std::uint32_t kKnownFlags =
                    NOVAFIX_PLUGIN_PRESENTATION_ADVANCED |
                    NOVAFIX_PLUGIN_PRESENTATION_RESTART_REQUIRED |
                    NOVAFIX_PLUGIN_PRESENTATION_EXPERIMENTAL |
                    NOVAFIX_PLUGIN_PRESENTATION_COLLAPSED_BY_DEFAULT;
                output->presentationFlags = extension.flags & kKnownFlags;
                presentationSeen = true;
            } else if (presentationSeen) {
                SetWarning(warning,
                           "duplicate presentation extension was ignored");
            } else {
                SetWarning(warning, "presentation extension is invalid");
            }
        } else if (header.version >=
                       NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION &&
                   header.type == NOVAFIX_PLUGIN_EXTENSION_VALUE_DISPLAY) {
            NovaFixPluginValueDisplayExtension extension{};
            if (!valueDisplaySeen &&
                TryCopyExtension(current, &extension,
                                 kValueDisplayExtensionV1Size,
                                 &exceptionCode)) {
                if (!CopyText(extension.prefix, kMaxValueAffixLength, {},
                              &valueDisplay->prefix) ||
                    !CopyText(extension.suffix, kMaxValueAffixLength, {},
                              &valueDisplay->suffix)) {
                    SetWarning(warning, "value display text was replaced");
                }
                if (extension.decimalPlaces <= 6u ||
                    extension.decimalPlaces ==
                        NOVAFIX_PLUGIN_DECIMALS_DEFAULT) {
                    valueDisplay->decimalPlaces = extension.decimalPlaces;
                } else {
                    SetWarning(warning,
                               "value display decimal count was ignored");
                }
                valueDisplay->present = true;
                valueDisplaySeen = true;
            } else if (valueDisplaySeen) {
                SetWarning(warning,
                           "duplicate value display extension was ignored");
            } else {
                SetWarning(warning, "value display extension is invalid");
            }
        }
        current = header.next;
    }
    if (current && count == kMaxExtensions) {
        SetWarning(warning, "extension limit reached");
        DisableExtensionChain(output, "Invalid extension chain: cycle or extension limit exceeded.");
    }
}

bool CopyChoiceOptions(const NovaFixPluginControl& source,
                       plugin_controls::Control* output,
                       std::string* warning) {
    if (!output) return false;
    const bool radio =
        source.type == NOVAFIX_PLUGIN_CONTROL_RADIO_GROUP;
    const char* const kind = radio ? "Radio group" : "Combo";
    if (!source.optionCount || !source.options) {
        Disable(output, std::string(kind) + " has no options");
        return true;
    }
    const std::uint32_t maximumOptions = radio
        ? NOVAFIX_PLUGIN_MAX_RADIO_OPTIONS
        : kMaxComboOptions;
    if (source.optionCount > maximumOptions) {
        Disable(output, std::string(kind) + " has too many options");
        return true;
    }
    if (radio && source.optionCount < 2u) {
        Disable(output, "Radio group needs at least two options");
        return true;
    }

    output->options.reserve(source.optionCount);
    for (std::uint32_t optionIndex = 0u;
         optionIndex < source.optionCount; ++optionIndex) {
        const char* optionAddress = nullptr;
        DWORD exceptionCode = 0;
        if (!TryCopyTextPointer(source.options, optionIndex, &optionAddress,
                                &exceptionCode)) {
            Disable(output, std::string(kind) +
                                " option list is unreadable");
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
                *warning = radio
                    ? "radio option text was replaced or truncated"
                    : "combo option text was replaced or truncated";
            }
        }
        output->options.push_back(std::move(option));
    }
    output->minimum = 0.0f;
    output->maximum = static_cast<float>(output->options.size() - 1u);
    output->step = 1.0f;
    return true;
}

bool CopyControl(const void* address, std::uint32_t stride,
                 std::uint32_t index,
                 plugin_controls::Control* output,
                 std::string* warning,
                 std::vector<const NovaFixPluginExtension*>& chain) {
    if (!address || !output) return false;
    NovaFixPluginControl source{};
    DWORD exceptionCode = 0;
    if (!TryCopyControl(address, stride, &source, &exceptionCode)) {
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
    ValueDisplay valueDisplay;
    ReadControlExtensions(
        source.extensions, output, &valueDisplay, warning, chain);
    if (!output->available) return true;

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

    if (valueDisplay.present &&
        source.type != NOVAFIX_PLUGIN_CONTROL_INT_SLIDER &&
        source.type != NOVAFIX_PLUGIN_CONTROL_FLOAT_SLIDER) {
        SetWarning(warning,
                   "value display extension was ignored for this control");
        valueDisplay = {};
    }
    if (output->readText &&
        source.type != NOVAFIX_PLUGIN_CONTROL_STATUS_TEXT) {
        SetWarning(warning,
                   "status text extension was ignored for this control");
        output->readText = nullptr;
        output->textUserData = nullptr;
    }
    if (output->readColor && source.type != NOVAFIX_PLUGIN_CONTROL_COLOR) {
        SetWarning(warning,
                   "color extension was ignored for this control");
        output->readColor = nullptr;
        output->writeColor = nullptr;
        output->colorUserData = nullptr;
    }
    if (source.type != NOVAFIX_PLUGIN_CONTROL_SECTION &&
        (output->presentationFlags &
         NOVAFIX_PLUGIN_PRESENTATION_COLLAPSED_BY_DEFAULT)) {
        SetWarning(warning,
                   "collapsed-by-default applies to sections only");
        output->presentationFlags &=
            ~NOVAFIX_PLUGIN_PRESENTATION_COLLAPSED_BY_DEFAULT;
    }

    if (source.type == NOVAFIX_PLUGIN_CONTROL_HEADER ||
        source.type == NOVAFIX_PLUGIN_CONTROL_SEPARATOR ||
        source.type == NOVAFIX_PLUGIN_CONTROL_TEXT ||
        source.type == NOVAFIX_PLUGIN_CONTROL_SECTION) {
        return true;
    }
    if (source.type == NOVAFIX_PLUGIN_CONTROL_STATUS_TEXT) {
        if (!output->readText) Disable(output, "Missing text callback");
        return true;
    }
    if (source.type == NOVAFIX_PLUGIN_CONTROL_COLOR) {
        if (!output->readColor || !output->writeColor) {
            Disable(output, "Missing color callback");
        }
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
    if (source.type == NOVAFIX_PLUGIN_CONTROL_COMBO ||
        source.type == NOVAFIX_PLUGIN_CONTROL_RADIO_GROUP) {
        return CopyChoiceOptions(source, output, warning);
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
    BuildValueFormat(output, valueDisplay,
                     source.type == NOVAFIX_PLUGIN_CONTROL_INT_SLIDER);
    return true;
}

} // namespace

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
        static_cast<std::uint32_t>(kLegacyControlSize);
    if (descriptor.structSize >= kDescriptorControlStrideSize) {
        controlStride = descriptor.controlStride
                            ? descriptor.controlStride
                            : controlStride;
        if (controlStride < kControlRequiredSize) {
            LogWarning("Add-on controls unavailable: source=%ls id=%s reason=invalid stride",
                       plugin.source.c_str(), plugin.id.c_str());
            return;
        }
    }

    plugin.controls.reserve(controlCount);
    std::unordered_map<const NovaFixPluginExtension*, std::size_t> extensionOwners;
    for (std::uint32_t index = 0u; index < controlCount; ++index) {
        const void* controlAddress = ElementAddress(
            descriptor.controls, index, controlStride);
        if (!controlAddress) break;
        plugin_controls::Control control{};
        std::string warning;
        std::vector<const NovaFixPluginExtension*> chain;
        if (!CopyControl(controlAddress, controlStride, index,
                         &control, &warning, chain)) {
            LogWarning("Add-on control skipped: source=%ls id=%s index=%u reason=%s",
                plugin.source.c_str(), plugin.id.c_str(), index,
                warning.empty() ? "unreadable descriptor"
                                : warning.c_str());
            continue;
        }
        for (const auto* extension : chain) {
            const auto [owner, inserted] = extensionOwners.emplace(
                extension, plugin.controls.size());
            if (!inserted && owner->second != plugin.controls.size()) {
                constexpr std::string_view issue =
                    "Extension instance reused across controls. Create a separate instance for each control.";
                DisableExtensionChain(&control, issue);
                DisableExtensionChain(&plugin.controls[owner->second], issue);
            }
        }
        if (!warning.empty()) {
            LogWarning("Add-on control fallback: source=%ls id=%s control=%s reason=%s",
                plugin.source.c_str(), plugin.id.c_str(),
                control.id.c_str(), warning.c_str());
        }
        if (std::any_of(
                plugin.controls.begin(), plugin.controls.end(),
                [&](const plugin_controls::Control& existing) {
                    return SameId(existing.id, control.id);
                })) {
            LogWarning("Add-on control disabled: source=%ls id=%s control=%s reason=duplicate id",
                       plugin.source.c_str(), plugin.id.c_str(),
                       control.id.c_str());
            Disable(&control, "Duplicate control ID");
        }
        plugin.controls.push_back(std::move(control));
    }
}

} // namespace novafix::compat::plugin_descriptor
