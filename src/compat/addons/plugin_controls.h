#pragma once

#include "novafix/plugin_api.h"

#include <windows.h>

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
    std::vector<std::string> options;
    bool available{true};
    std::string issue;
    mutable bool callbackFailed{};
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

// LR menu callbacks run on the window thread; failure disables only that item
bool ReadMenuState(const Plugin& plugin, const NativeMenuItem& item,
                   uint32_t* state);
bool ActivateMenuItem(const Plugin& plugin, const NativeMenuItem& item);

} // namespace novafix::compat::plugin_controls
