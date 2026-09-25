#pragma once

#include "novafix/plugin_api.h"

#include <windows.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace novafix::compat::plugin_controls {

struct Control {
    uint32_t type{};
    std::string id;
    std::string label;
    std::string description;
    float minimum{};
    float maximum{};
    float step{};
    NovaFixPluginReadValueFn readValue{};
    NovaFixPluginWriteValueFn writeValue{};
    NovaFixPluginActivateControlFn activate{};
    void* userData{};
    NovaFixPluginReadControlStateFn readState{};
    void* stateUserData{};
    NovaFixPluginReadTextFn readText{};
    void* textUserData{};
    uint32_t refreshIntervalMs{};
    NovaFixPluginReadColorFn readColor{};
    NovaFixPluginWriteColorFn writeColor{};
    void* colorUserData{};
    uint32_t colorFlags{};
    uint32_t presentationFlags{};
    std::string valueFormat;
    std::vector<std::string> options;
    bool available{true};
    std::string issue;
    mutable bool callbackFailed{};
    mutable bool stateCallbackFailed{};
    mutable bool textCallbackFailed{};
    mutable bool colorCallbackFailed{};
    mutable bool textCacheValid{};
    mutable std::string cachedText;
    mutable std::chrono::steady_clock::time_point nextTextRefresh{};
};

struct NativeMenuItem {
    uint32_t type{};
    std::string id;
    std::string label;
    int32_t parentIndex{-1};
    NovaFixPluginReadMenuStateFn readState{};
    NovaFixPluginActivateMenuItemFn activate{};
    void* userData{};
    bool available{true};
    std::string issue;
    mutable bool callbackFailed{};
};

struct NativeMenu {
    std::string id;
    std::string label;
    std::vector<NativeMenuItem> items;
};

struct Plugin {
    HMODULE module{};
    std::wstring source;
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string website;
    NovaFixPluginColor headerColor{};
    bool hasHeaderColor{};
    bool headerColorAppliesToContent{};
    std::vector<Control> controls;
    std::vector<NativeMenu> nativeMenus;
};

// Configure before add-on discovery so Query sees the current game
void ConfigureHost(NovaFixPluginGame game, const char* gameName,
                   const char* novaFixVersion,
                   NovaFixPluginShowNativeMessageFn showNativeMessage = nullptr,
                   NovaFixPluginShowNativeMessageExFn showNativeMessageEx = nullptr);

// Descriptor text is copied; callbacks remain owned by the loaded add-on
bool Discover(HMODULE module, std::wstring_view source);
bool HasNativeMenus();
const std::vector<Plugin>& CurrentPlugins();

// A callback exception disables that control, not the add-on DLL
bool ReadValue(const Plugin& plugin, const Control& control, float* value);
bool WriteValue(const Plugin& plugin, const Control& control, float value);
bool ActivateControl(const Plugin& plugin, const Control& control);
uint32_t ReadControlState(const Plugin& plugin, const Control& control);
bool ReadText(const Plugin& plugin, const Control& control,
              const char** value);
bool ReadColor(const Plugin& plugin, const Control& control,
               NovaFixPluginColor* value);
bool WriteColor(const Plugin& plugin, const Control& control,
                const NovaFixPluginColor& value);

// LR menu callbacks run on the window thread; failure disables only that item
bool ReadMenuState(const Plugin& plugin, const NativeMenuItem& item,
                   uint32_t* state);
bool ActivateMenuItem(const Plugin& plugin, const NativeMenuItem& item);

} // namespace novafix::compat::plugin_controls
