#include "compat/addons/plugin_controls.h"
#include "compat/addons/plugin_descriptor.h"
#include "compat/addons/plugin_services.h"

#include "common/guarded_call.h"
#include "diagnostics/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace novafix::compat::plugin_controls {
namespace {

std::vector<Plugin> g_plugins;
bool g_hasNativeMenus{};
std::string g_hostGameName{"Unknown application"};
std::string g_hostVersion{"unknown"};
NovaFixPluginHost g_host{
    sizeof(NovaFixPluginHost), NOVAFIX_PLUGIN_API_VERSION,
    NOVAFIX_PLUGIN_GAME_UNKNOWN, g_hostGameName.c_str(),
    g_hostVersion.c_str(), nullptr, nullptr, nullptr,
    &plugin_services::GetService};

bool TryQuery(NovaFixPluginQueryFn query,
              const NovaFixPluginDescriptor** descriptor,
              DWORD* exceptionCode) noexcept {
    if (!query || !descriptor || !exceptionCode) return false;
    return guarded_call::Run([&] {
        *descriptor = query(&g_host);
    }, exceptionCode);
}

bool TryRead(NovaFixPluginReadValueFn callback, void* userData, float* value,
             DWORD* exceptionCode) noexcept {
    if (!callback || !value || !exceptionCode) return false;
    return guarded_call::Run([&] {
        *value = callback(userData);
    }, exceptionCode);
}

bool TryWrite(NovaFixPluginWriteValueFn callback, void* userData, float value,
              DWORD* exceptionCode) noexcept {
    if (!callback || !exceptionCode) return false;
    return guarded_call::Run([&] {
        callback(userData, value);
    }, exceptionCode);
}

bool TryActivateControl(NovaFixPluginActivateControlFn callback,
                        void* userData, DWORD* exceptionCode) noexcept {
    if (!callback || !exceptionCode) return false;
    return guarded_call::Run([&] {
        callback(userData);
    }, exceptionCode);
}

bool TryReadControlState(NovaFixPluginReadControlStateFn callback,
                         void* userData, std::uint32_t* state,
                         DWORD* exceptionCode) noexcept {
    if (!callback || !state || !exceptionCode) return false;
    return guarded_call::Run([&] {
        *state = callback(userData);
    }, exceptionCode);
}

bool TryReadText(NovaFixPluginReadTextFn callback, void* userData,
                 char* output, std::uint32_t capacity,
                 std::uint32_t* length, DWORD* exceptionCode) noexcept {
    if (!callback || !output || !capacity || !length || !exceptionCode) {
        return false;
    }
    return guarded_call::Run([&] {
        *length = callback(userData, output, capacity);
    }, exceptionCode);
}

bool TryReadColor(NovaFixPluginReadColorFn callback, void* userData,
                  NovaFixPluginColor* value,
                  DWORD* exceptionCode) noexcept {
    if (!callback || !value || !exceptionCode) return false;
    return guarded_call::Run([&] {
        callback(userData, value);
    }, exceptionCode);
}

bool TryWriteColor(NovaFixPluginWriteColorFn callback, void* userData,
                   const NovaFixPluginColor* value,
                   DWORD* exceptionCode) noexcept {
    if (!callback || !value || !exceptionCode) return false;
    return guarded_call::Run([&] {
        callback(userData, value);
    }, exceptionCode);
}

bool TryReadMenuState(NovaFixPluginReadMenuStateFn callback, void* userData,
                      uint32_t* state, DWORD* exceptionCode) noexcept {
    if (!callback || !state || !exceptionCode) return false;
    return guarded_call::Run([&] {
        *state = callback(userData);
    }, exceptionCode);
}

bool TryActivateMenuItem(NovaFixPluginActivateMenuItemFn callback,
                         void* userData, DWORD* exceptionCode) noexcept {
    if (!callback || !exceptionCode) return false;
    return guarded_call::Run([&] {
        callback(userData);
    }, exceptionCode);
}

FARPROC NamedExport(HMODULE module, const char* name,
                    const char* decoratedName) {
    if (!module) return nullptr;
    FARPROC query = GetProcAddress(module, name);
    // Some x86 toolchains keep the C symbol's leading underscore
    if (!query) query = GetProcAddress(module, decoratedName);
    return query;
}

void MarkCallbackFailed(const Plugin& plugin, const Control& control,
                        const char* operation, DWORD exceptionCode) {
    if (control.callbackFailed) return;
    control.callbackFailed = true;
    if (exceptionCode) {
        Log("Addon UI callback disabled: addon=%s control=%s operation=%s "
            "exception=0x%08lX (the DLL remains loaded)",
            plugin.id.c_str(), control.id.c_str(), operation, exceptionCode);
    } else {
        Log("Addon UI callback disabled: addon=%s control=%s operation=%s "
            "reason=invalid value (the DLL remains loaded)",
            plugin.id.c_str(), control.id.c_str(), operation);
    }
}

void MarkSpecialCallbackFailed(const Plugin& plugin, const Control& control,
                               bool* failed, const char* operation,
                               DWORD exceptionCode) {
    if (!failed || *failed) return;
    *failed = true;
    if (exceptionCode) {
        LogWarning("Add-on UI callback disabled: addon=%s control=%s operation=%s exception=0x%08lX",
                   plugin.id.c_str(), control.id.c_str(), operation,
                   exceptionCode);
    } else {
        LogWarning("Add-on UI callback disabled: addon=%s control=%s operation=%s reason=invalid value",
                   plugin.id.c_str(), control.id.c_str(), operation);
    }
}

void MarkMenuCallbackFailed(const Plugin& plugin, const NativeMenuItem& item,
                            const char* operation, DWORD exceptionCode) {
    if (item.callbackFailed) return;
    item.callbackFailed = true;
    Log("Addon native menu callback disabled: addon=%s item=%s operation=%s "
        "exception=0x%08lX (the DLL remains loaded)", plugin.id.c_str(),
        item.id.c_str(), operation, exceptionCode);
}

} // namespace

void ConfigureHost(NovaFixPluginGame game, const char* gameName,
                   const char* novaFixVersion,
                   NovaFixPluginShowNativeMessageFn showNativeMessage,
                   NovaFixPluginShowNativeMessageExFn showNativeMessageEx) {
    // Query may retain these pointers, so keep our own copies
    g_hostGameName = gameName && *gameName ? gameName : "Unknown application";
    g_hostVersion = novaFixVersion && *novaFixVersion ? novaFixVersion : "unknown";
    g_host.structSize = sizeof(g_host);
    g_host.apiVersion = NOVAFIX_PLUGIN_API_VERSION;
    g_host.game = game;
    g_host.gameName = g_hostGameName.c_str();
    g_host.novaFixVersion = g_hostVersion.c_str();
    g_host.showNativeMessage = showNativeMessage;
    g_host.showNativeMessageEx = showNativeMessageEx;
    g_host.hostContext = nullptr;
    g_host.getService = &plugin_services::GetService;
}

bool Discover(HMODULE module, std::wstring_view source) {
    const NovaFixPluginDescriptor* descriptorAddress = nullptr;
    DWORD exceptionCode = 0;
    const FARPROC queryAddress = NamedExport(
        module, NOVAFIX_PLUGIN_QUERY_EXPORT, "_NovaFixPlugin_Query");
    if (!queryAddress) return false;
    if (!TryQuery(reinterpret_cast<NovaFixPluginQueryFn>(queryAddress),
                  &descriptorAddress, &exceptionCode)) {
        const std::wstring sourceText(source);
        LogWarning("Addon UI query failed: source=%ls exception=0x%08lX "
            "(the DLL remains loaded)", sourceText.c_str(), exceptionCode);
        return false;
    }
    if (!descriptorAddress) return false;

    NovaFixPluginDescriptor descriptor{};
    if (!plugin_descriptor::CopyDescriptor(
            descriptorAddress, &descriptor, &exceptionCode) ||
        descriptor.apiVersion != NOVAFIX_PLUGIN_API_VERSION) {
        const std::wstring sourceText(source);
        LogWarning("Add-on API descriptor rejected: source=%ls",
            sourceText.c_str());
        return false;
    }

    Plugin plugin{};
    plugin.module = module;
    plugin.source = source;
    plugin_descriptor::ReadIdentity(
        descriptor, source, g_plugins.size(), plugin);

    if (std::any_of(g_plugins.begin(), g_plugins.end(), [&](const Plugin& item) {
            return item.module == module ||
                plugin_descriptor::SameId(item.id, plugin.id);
        })) {
        Log("Add-on API already registered: source=%ls id=%s",
            plugin.source.c_str(), plugin.id.c_str());
        return false;
    }

    plugin_descriptor::ReadControls(descriptor, plugin);
    plugin_descriptor::ReadNativeMenus(descriptor, plugin);
    Log("Add-on API registered: source=%ls id=%s controls=%zu nativeMenus=%zu game=%u",
        plugin.source.c_str(), plugin.id.c_str(), plugin.controls.size(),
        plugin.nativeMenus.size(), g_host.game);
    g_hasNativeMenus = g_hasNativeMenus || !plugin.nativeMenus.empty();
    g_plugins.push_back(std::move(plugin));
    return true;
}

bool HasNativeMenus() {
    return g_hasNativeMenus;
}

const std::vector<Plugin>& CurrentPlugins() {
    return g_plugins;
}

bool ReadValue(const Plugin& plugin, const Control& control, float* value) {
    if (!value || !control.available || control.callbackFailed ||
        !control.readValue) {
        return false;
    }
    DWORD exceptionCode = 0;
    float result = 0.0f;
    if (!TryRead(control.readValue, control.userData, &result, &exceptionCode) ||
        !std::isfinite(result)) {
        MarkCallbackFailed(plugin, control, "read", exceptionCode);
        return false;
    }
    *value = result;
    return true;
}

bool WriteValue(const Plugin& plugin, const Control& control, float value) {
    if (!std::isfinite(value) || !control.available || control.callbackFailed ||
        !control.writeValue) {
        return false;
    }
    DWORD exceptionCode = 0;
    if (!TryWrite(control.writeValue, control.userData, value, &exceptionCode)) {
        MarkCallbackFailed(plugin, control, "write", exceptionCode);
        return false;
    }
    return true;
}

bool ActivateControl(const Plugin& plugin, const Control& control) {
    if (!control.available || control.callbackFailed || !control.activate) {
        return false;
    }
    DWORD exceptionCode = 0;
    if (!TryActivateControl(control.activate, control.userData,
                            &exceptionCode)) {
        MarkCallbackFailed(plugin, control, "activate", exceptionCode);
        return false;
    }
    return true;
}

uint32_t ReadControlState(const Plugin& plugin, const Control& control) {
    constexpr std::uint32_t kDefault =
        NOVAFIX_PLUGIN_CONTROL_STATE_VISIBLE |
        NOVAFIX_PLUGIN_CONTROL_STATE_ENABLED;
    if (!control.readState) return kDefault;
    if (control.stateCallbackFailed) {
        return NOVAFIX_PLUGIN_CONTROL_STATE_VISIBLE;
    }

    DWORD exceptionCode{};
    std::uint32_t state{};
    if (!TryReadControlState(control.readState, control.stateUserData, &state,
                             &exceptionCode)) {
        MarkSpecialCallbackFailed(plugin, control,
                                  &control.stateCallbackFailed,
                                  "read-state", exceptionCode);
        return NOVAFIX_PLUGIN_CONTROL_STATE_VISIBLE;
    }
    return state & (NOVAFIX_PLUGIN_CONTROL_STATE_VISIBLE |
                    NOVAFIX_PLUGIN_CONTROL_STATE_ENABLED |
                    NOVAFIX_PLUGIN_CONTROL_STATE_READ_ONLY |
                    NOVAFIX_PLUGIN_CONTROL_STATE_BUSY);
}

bool ReadText(const Plugin& plugin, const Control& control,
              const char** value) {
    if (!value || !control.available || !control.readText ||
        control.textCallbackFailed) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (control.textCacheValid && now < control.nextTextRefresh) {
        *value = control.cachedText.c_str();
        return true;
    }

    constexpr std::uint32_t kCapacity =
        NOVAFIX_PLUGIN_MAX_STATUS_TEXT_BYTES + 1u;
    std::array<char, kCapacity> buffer{};
    std::uint32_t length{};
    DWORD exceptionCode{};
    if (!TryReadText(control.readText, control.textUserData, buffer.data(),
                     kCapacity, &length, &exceptionCode)) {
        MarkSpecialCallbackFailed(plugin, control,
                                  &control.textCallbackFailed,
                                  "read-text", exceptionCode);
        return false;
    }
    length = std::min(length, kCapacity - 1u);
    const auto terminator = std::find(
        buffer.begin(), buffer.begin() + length, '\0');
    length = static_cast<std::uint32_t>(
        std::distance(buffer.begin(), terminator));
    buffer[length] = '\0';
    control.cachedText.assign(buffer.data(), length);
    control.textCacheValid = true;
    control.nextTextRefresh = now +
        std::chrono::milliseconds(control.refreshIntervalMs);
    *value = control.cachedText.c_str();
    return true;
}

bool ReadColor(const Plugin& plugin, const Control& control,
               NovaFixPluginColor* value) {
    if (!value || !control.available || !control.readColor ||
        control.colorCallbackFailed) {
        return false;
    }
    DWORD exceptionCode{};
    NovaFixPluginColor result{};
    if (!TryReadColor(control.readColor, control.colorUserData, &result,
                      &exceptionCode) ||
        !std::isfinite(result.red) || !std::isfinite(result.green) ||
        !std::isfinite(result.blue) || !std::isfinite(result.alpha) ||
        result.red < 0.0f || result.red > 1.0f ||
        result.green < 0.0f || result.green > 1.0f ||
        result.blue < 0.0f || result.blue > 1.0f ||
        result.alpha < 0.0f || result.alpha > 1.0f) {
        MarkSpecialCallbackFailed(plugin, control,
                                  &control.colorCallbackFailed,
                                  "read-color", exceptionCode);
        return false;
    }
    *value = result;
    return true;
}

bool WriteColor(const Plugin& plugin, const Control& control,
                const NovaFixPluginColor& value) {
    if (!control.available || !control.writeColor ||
        control.colorCallbackFailed || !std::isfinite(value.red) ||
        !std::isfinite(value.green) || !std::isfinite(value.blue) ||
        !std::isfinite(value.alpha) || value.red < 0.0f ||
        value.red > 1.0f || value.green < 0.0f || value.green > 1.0f ||
        value.blue < 0.0f || value.blue > 1.0f || value.alpha < 0.0f ||
        value.alpha > 1.0f) {
        return false;
    }
    DWORD exceptionCode{};
    if (!TryWriteColor(control.writeColor, control.colorUserData, &value,
                       &exceptionCode)) {
        MarkSpecialCallbackFailed(plugin, control,
                                  &control.colorCallbackFailed,
                                  "write-color", exceptionCode);
        return false;
    }
    return true;
}

bool ReadMenuState(const Plugin& plugin, const NativeMenuItem& item,
                   uint32_t* state) {
    if (!state || !item.available || item.callbackFailed) return false;
    if (!item.readState) {
        *state = NOVAFIX_PLUGIN_NATIVE_MENU_ENABLED;
        return true;
    }
    DWORD exceptionCode = 0;
    uint32_t result = 0;
    if (!TryReadMenuState(item.readState, item.userData, &result,
                          &exceptionCode)) {
        MarkMenuCallbackFailed(plugin, item, "read", exceptionCode);
        return false;
    }
    *state = result;
    return true;
}

bool ActivateMenuItem(const Plugin& plugin, const NativeMenuItem& item) {
    if (!item.available || item.callbackFailed || !item.activate) return false;
    DWORD exceptionCode = 0;
    if (!TryActivateMenuItem(item.activate, item.userData, &exceptionCode)) {
        MarkMenuCallbackFailed(plugin, item, "activate", exceptionCode);
        return false;
    }
    return true;
}

} // namespace novafix::compat::plugin_controls
