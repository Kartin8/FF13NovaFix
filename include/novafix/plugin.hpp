#pragma once

#include "novafix/plugin_api.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace novafix {

struct PluginMetadata {
    const char* author{};
    const char* version{};
    const char* description{};
    const char* website{};
    NovaFixPluginColor headerColor{};
    std::uint32_t headerColorFlags{};
};

struct ControlOptions {
    NovaFixPluginReadControlStateFn readState{};
    void* stateUserData{};
    std::uint32_t presentationFlags{};
    const char* valuePrefix{};
    const char* valueSuffix{};
    std::uint32_t decimalPlaces{NOVAFIX_PLUGIN_DECIMALS_DEFAULT};

    ControlOptions& state(NovaFixPluginReadControlStateFn callback,
                          void* userData = nullptr) noexcept {
        readState = callback;
        stateUserData = userData;
        return *this;
    }

    ControlOptions& presentation(std::uint32_t flags) noexcept {
        presentationFlags = flags;
        return *this;
    }

    ControlOptions& valueDisplay(
        const char* prefix, const char* suffix,
        std::uint32_t decimals = NOVAFIX_PLUGIN_DECIMALS_DEFAULT) noexcept {
        valuePrefix = prefix;
        valueSuffix = suffix;
        decimalPlaces = decimals;
        return *this;
    }
};

class Control {
public:
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;
    Control(Control&&) = delete;
    Control& operator=(Control&&) = delete;

    static Control raw(NovaFixPluginControl descriptor,
                       ControlOptions options = {}) noexcept {
        descriptor.structSize = sizeof(NovaFixPluginControl);
        return Control(descriptor, options);
    }

    static Control checkbox(
        const char* id, const char* label, const char* description,
        NovaFixPluginReadValueFn readValue,
        NovaFixPluginWriteValueFn writeValue, void* userData = nullptr,
        ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeCheckbox(
                           id, label, description, readValue, writeValue,
                           userData),
                       options);
    }

    static Control intSlider(
        const char* id, const char* label, const char* description,
        float minimum, float maximum, float step,
        NovaFixPluginReadValueFn readValue,
        NovaFixPluginWriteValueFn writeValue, void* userData = nullptr,
        ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeIntSlider(
                           id, label, description, minimum, maximum, step,
                           readValue, writeValue, userData),
                       options);
    }

    static Control floatSlider(
        const char* id, const char* label, const char* description,
        float minimum, float maximum, float step,
        NovaFixPluginReadValueFn readValue,
        NovaFixPluginWriteValueFn writeValue, void* userData = nullptr,
        ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeFloatSlider(
                           id, label, description, minimum, maximum, step,
                           readValue, writeValue, userData),
                       options);
    }

    static Control button(
        const char* id, const char* label, const char* description,
        NovaFixPluginActivateControlFn activate, void* userData = nullptr,
        ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeButton(
                           id, label, description, activate, userData),
                       options);
    }

    static Control combo(
        const char* id, const char* label, const char* description,
        std::uint32_t optionCount, const char* const* choices,
        NovaFixPluginReadValueFn readValue,
        NovaFixPluginWriteValueFn writeValue, void* userData = nullptr,
        ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeCombo(
                           id, label, description, optionCount, choices,
                           readValue, writeValue, userData),
                       options);
    }

    template <std::size_t OptionCount>
    static Control combo(
        const char* id, const char* label, const char* description,
        const char* const (&choices)[OptionCount],
        NovaFixPluginReadValueFn readValue,
        NovaFixPluginWriteValueFn writeValue, void* userData = nullptr,
        ControlOptions options = {}) noexcept {
        return combo(id, label, description,
                     static_cast<std::uint32_t>(OptionCount), choices,
                     readValue, writeValue, userData, options);
    }

    static Control radioGroup(
        const char* id, const char* label, const char* description,
        std::uint32_t optionCount, const char* const* choices,
        NovaFixPluginReadValueFn readValue,
        NovaFixPluginWriteValueFn writeValue, void* userData = nullptr,
        ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeRadioGroup(
                           id, label, description, optionCount, choices,
                           readValue, writeValue, userData),
                       options);
    }

    template <std::size_t OptionCount>
    static Control radioGroup(
        const char* id, const char* label, const char* description,
        const char* const (&choices)[OptionCount],
        NovaFixPluginReadValueFn readValue,
        NovaFixPluginWriteValueFn writeValue, void* userData = nullptr,
        ControlOptions options = {}) noexcept {
        return radioGroup(
            id, label, description,
            static_cast<std::uint32_t>(OptionCount), choices,
            readValue, writeValue, userData, options);
    }

    static Control header(const char* id, const char* label,
                          ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeHeader(id, label), options);
    }

    static Control separator(const char* id,
                             ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeSeparator(id), options);
    }

    static Control text(const char* id, const char* value,
                        ControlOptions options = {}) noexcept {
        return Control(NovaFixPlugin_MakeText(id, value), options);
    }

    static Control section(const char* id, const char* label,
                           const char* description = nullptr,
                           ControlOptions options = {}) noexcept {
        return Control(
            NovaFixPlugin_MakeSection(id, label, description), options);
    }

    static Control statusText(
        const char* id, const char* label, const char* description,
        NovaFixPluginReadTextFn readText, void* userData = nullptr,
        std::uint32_t refreshIntervalMs = 0u,
        ControlOptions options = {}) noexcept {
        NovaFixPluginControl control = NovaFixPlugin_MakeText(id, label);
        control.type = NOVAFIX_PLUGIN_CONTROL_STATUS_TEXT;
        control.description = description;
        return Control(control, options, readText, userData,
                       refreshIntervalMs);
    }

    static Control color(
        const char* id, const char* label, const char* description,
        NovaFixPluginReadColorFn readColor,
        NovaFixPluginWriteColorFn writeColor, void* userData = nullptr,
        std::uint32_t flags = 0u,
        ControlOptions options = {}) noexcept {
        NovaFixPluginControl control = NovaFixPlugin_MakeText(id, label);
        control.type = NOVAFIX_PLUGIN_CONTROL_COLOR;
        control.description = description;
        return Control(control, options, readColor, writeColor, userData,
                       flags);
    }

    NovaFixPluginControl& descriptor() noexcept { return descriptor_; }
    const NovaFixPluginControl& descriptor() const noexcept {
        return descriptor_;
    }

private:
    explicit Control(NovaFixPluginControl descriptor,
                     ControlOptions options) noexcept
        : descriptor_(descriptor) {
        addCommonExtensions(options);
    }

    Control(NovaFixPluginControl descriptor, ControlOptions options,
            NovaFixPluginReadTextFn readText, void* userData,
            std::uint32_t refreshIntervalMs) noexcept
        : descriptor_(descriptor),
          status_(NovaFixPlugin_MakeStatusTextExtension(
              readText, userData, refreshIntervalMs)) {
        addCommonExtensions(options);
        NovaFixPlugin_AddControlExtension(&descriptor_, &status_.header);
    }

    Control(NovaFixPluginControl descriptor, ControlOptions options,
            NovaFixPluginReadColorFn readColor,
            NovaFixPluginWriteColorFn writeColor, void* userData,
            std::uint32_t flags) noexcept
        : descriptor_(descriptor),
          color_(NovaFixPlugin_MakeColorExtension(
              readColor, writeColor, userData, flags)) {
        addCommonExtensions(options);
        NovaFixPlugin_AddControlExtension(&descriptor_, &color_.header);
    }

    void addCommonExtensions(ControlOptions options) noexcept {
        if (options.readState) {
            state_ = NovaFixPlugin_MakeControlStateExtension(
                options.readState, options.stateUserData);
            NovaFixPlugin_AddControlExtension(&descriptor_, &state_.header);
        }
        if (options.presentationFlags) {
            presentation_ = NovaFixPlugin_MakePresentationExtension(
                options.presentationFlags);
            NovaFixPlugin_AddControlExtension(
                &descriptor_, &presentation_.header);
        }
        if (options.valuePrefix || options.valueSuffix ||
            options.decimalPlaces != NOVAFIX_PLUGIN_DECIMALS_DEFAULT) {
            valueDisplay_ = NovaFixPlugin_MakeValueDisplayExtension(
                options.valuePrefix, options.valueSuffix,
                options.decimalPlaces);
            NovaFixPlugin_AddControlExtension(
                &descriptor_, &valueDisplay_.header);
        }
    }

    NovaFixPluginControl descriptor_{};
    NovaFixPluginControlStateExtension state_{};
    NovaFixPluginStatusTextExtension status_{};
    NovaFixPluginColorExtension color_{};
    NovaFixPluginPresentationExtension presentation_{};
    NovaFixPluginValueDisplayExtension valueDisplay_{};

    friend class Plugin;
};

static_assert(std::is_standard_layout_v<Control>);

class Plugin {
public:
    template <std::size_t ControlCount>
    Plugin(const char* id, const char* name,
           const NovaFixPluginControl (&controls)[ControlCount]) noexcept
        : descriptor_(NovaFixPlugin_MakeDescriptor(
              id, name, static_cast<std::uint32_t>(ControlCount), controls)) {}

    template <std::size_t ControlCount>
    Plugin(const char* id, const char* name,
           const NovaFixPluginControl (&controls)[ControlCount],
           PluginMetadata metadata) noexcept
        : Plugin(id, name, controls) {
        this->metadata(metadata.author, metadata.version,
                       metadata.description, metadata.website)
            .headerColor(metadata.headerColor, metadata.headerColorFlags);
    }

    template <std::size_t ControlCount>
    Plugin(const char* id, const char* name,
           const Control (&controls)[ControlCount]) noexcept
        : descriptor_(NovaFixPlugin_MakeDescriptor(
              id, name, static_cast<std::uint32_t>(ControlCount),
              &controls[0].descriptor_)) {
        descriptor_.controlStride = static_cast<std::uint32_t>(
            sizeof(Control));
    }

    template <std::size_t ControlCount>
    Plugin(const char* id, const char* name,
           const Control (&controls)[ControlCount],
           PluginMetadata metadata) noexcept
        : Plugin(id, name, controls) {
        this->metadata(metadata.author, metadata.version,
                       metadata.description, metadata.website)
            .headerColor(metadata.headerColor, metadata.headerColorFlags);
    }

    Plugin(const char* id, const char* name) noexcept
        : descriptor_(NovaFixPlugin_MakeDescriptor(id, name, 0u, nullptr)) {}

    Plugin(const char* id, const char* name,
           PluginMetadata metadata) noexcept
        : Plugin(id, name) {
        this->metadata(metadata.author, metadata.version,
                       metadata.description, metadata.website)
            .headerColor(metadata.headerColor, metadata.headerColorFlags);
    }

    Plugin& metadata(const char* author, const char* version,
                     const char* description = nullptr,
                     const char* website = nullptr) noexcept {
        NovaFixPlugin_SetMetadata(&descriptor_, author, version, description,
                                  website);
        return *this;
    }

    Plugin& headerColor(NovaFixPluginColor color,
                        std::uint32_t flags = 0u) noexcept {
        NovaFixPlugin_SetHeaderColor(&descriptor_, color, flags);
        return *this;
    }

    template <std::size_t MenuCount>
    Plugin& nativeMenus(
        const NovaFixPluginNativeMenu (&menus)[MenuCount]) noexcept {
        NovaFixPlugin_SetNativeMenus(
            &descriptor_, static_cast<std::uint32_t>(MenuCount), menus);
        return *this;
    }

    NovaFixPluginDescriptor& descriptor() noexcept { return descriptor_; }
    const NovaFixPluginDescriptor& descriptor() const noexcept {
        return descriptor_;
    }

    const NovaFixPluginHost* host() const noexcept { return host_; }

    std::uint32_t game() const noexcept {
        return host_ ? host_->game
                     : static_cast<std::uint32_t>(
                           NOVAFIX_PLUGIN_GAME_UNKNOWN);
    }

    const char* gameName() const noexcept {
        return host_ && host_->gameName ? host_->gameName : "Unknown game";
    }

    const char* novaFixVersion() const noexcept {
        return host_ && host_->novaFixVersion
                   ? host_->novaFixVersion
                   : "unknown";
    }

    const void* service(const char* id,
                        std::uint32_t minimumVersion = 1u) const noexcept {
        return NovaFixPlugin_GetService(host_, id, minimumVersion);
    }

    std::uint32_t showNativeMessage(const char* text) const noexcept {
        return NovaFixPlugin_ShowNativeMessage(host_, text);
    }

    std::uint32_t showNativeMessage(
        const char* text,
        const NovaFixPluginNativeMessageOptions& options) const noexcept {
        return NovaFixPlugin_ShowNativeMessageEx(host_, text, &options);
    }

    const NovaFixPluginNativeNotificationService*
    nativeNotifications() const noexcept {
        return NovaFixPlugin_GetNativeNotificationService(host_);
    }

    std::uint32_t nativeNotificationCapabilities() const noexcept {
        return NovaFixPlugin_GetNativeNotificationCapabilities(host_);
    }

    std::uint32_t notify(
        const char* text,
        const NovaFixPluginNativeNotificationOptions& options) const noexcept {
        return NovaFixPlugin_Notify(host_, text, &options);
    }

    std::uint32_t notifyTimed(
        const char* text, std::uint32_t durationMs,
        std::uint32_t delayMs = 0u,
        NovaFixPluginNativeNotificationPosition position =
            NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_DEFAULT) const
        noexcept {
        NovaFixPluginNativeNotificationOptions options =
            NovaFixPlugin_MakeTimedNotificationOptions(durationMs);
        options.delayMs = delayMs;
        options.position = position;
        return notify(text, options);
    }

    std::uint32_t notifyAcknowledged(
        const char* text, std::uint32_t delayMs = 0u,
        NovaFixPluginNativeNotificationPosition position =
            NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_DEFAULT) const
        noexcept {
        NovaFixPluginNativeNotificationOptions options =
            NovaFixPlugin_MakeAcknowledgementNotificationOptions();
        options.delayMs = delayMs;
        options.position = position;
        return notify(text, options);
    }

    std::uint32_t log(std::uint32_t level,
                      const char* message) const noexcept {
        return NovaFixPlugin_Log(host_, level, descriptor_.id, message);
    }

    std::uint32_t debug(const char* message) const noexcept {
        return log(NOVAFIX_PLUGIN_LOG_DEBUG, message);
    }

    std::uint32_t info(const char* message) const noexcept {
        return log(NOVAFIX_PLUGIN_LOG_INFO, message);
    }

    std::uint32_t warning(const char* message) const noexcept {
        return log(NOVAFIX_PLUGIN_LOG_WARNING, message);
    }

    std::uint32_t error(const char* message) const noexcept {
        return log(NOVAFIX_PLUGIN_LOG_ERROR, message);
    }

    const NovaFixPluginDescriptor* query(
        const NovaFixPluginHost* host) noexcept {
        constexpr std::size_t requiredHostSize =
            offsetof(NovaFixPluginHost, novaFixVersion) +
            sizeof(const char*);
        if (!host || host->structSize < requiredHostSize ||
            host->apiVersion != NOVAFIX_PLUGIN_ABI_VERSION) {
            return nullptr;
        }
        host_ = host;
        return &descriptor_;
    }

private:
    static_assert(offsetof(Control, descriptor_) == 0u);
    static_assert(sizeof(Control) <= UINT32_MAX);

    NovaFixPluginDescriptor descriptor_{};
    const NovaFixPluginHost* host_{};
};

} // namespace novafix

#define NOVAFIX_PLUGIN_IMPLEMENT(instance)                                  \
    NOVAFIX_PLUGIN_EXPORT const NovaFixPluginDescriptor*                    \
        NOVAFIX_PLUGIN_CALL NovaFixPlugin_Query(                            \
            const NovaFixPluginHost* host) {                                \
        return (instance).query(host);                                      \
    }
