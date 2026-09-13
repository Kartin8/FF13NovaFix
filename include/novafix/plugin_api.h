#pragma once

#include <stddef.h>
#include <stdint.h>

#define NOVAFIX_PLUGIN_API_VERSION 1u
#define NOVAFIX_PLUGIN_QUERY_EXPORT "NovaFixPlugin_Query"

#if defined(_MSC_VER)
#define NOVAFIX_PLUGIN_CALL __cdecl
#define NOVAFIX_PLUGIN_DLLEXPORT __declspec(dllexport)
#else
#define NOVAFIX_PLUGIN_CALL __attribute__((cdecl))
#define NOVAFIX_PLUGIN_DLLEXPORT __attribute__((dllexport))
#endif

#ifdef __cplusplus
#define NOVAFIX_PLUGIN_EXTERN_C extern "C"
#else
#define NOVAFIX_PLUGIN_EXTERN_C extern
#endif

#define NOVAFIX_PLUGIN_EXPORT NOVAFIX_PLUGIN_EXTERN_C NOVAFIX_PLUGIN_DLLEXPORT

typedef enum NovaFixPluginControlType {
    NOVAFIX_PLUGIN_CONTROL_CHECKBOX = 1,
    NOVAFIX_PLUGIN_CONTROL_INT_SLIDER = 2,
    NOVAFIX_PLUGIN_CONTROL_FLOAT_SLIDER = 3,
    NOVAFIX_PLUGIN_CONTROL_BUTTON = 4,
    NOVAFIX_PLUGIN_CONTROL_COMBO = 5,
    NOVAFIX_PLUGIN_CONTROL_HEADER = 6,
    NOVAFIX_PLUGIN_CONTROL_SEPARATOR = 7,
    NOVAFIX_PLUGIN_CONTROL_TEXT = 8
} NovaFixPluginControlType;

// Ignore unrecognized game values for forward compatibility
typedef enum NovaFixPluginGame {
    NOVAFIX_PLUGIN_GAME_UNKNOWN = 0,
    NOVAFIX_PLUGIN_GAME_FF13 = 1,
    NOVAFIX_PLUGIN_GAME_FF13_2 = 2,
    NOVAFIX_PLUGIN_GAME_LIGHTNING_RETURNS = 3
} NovaFixPluginGame;

typedef enum NovaFixPluginResult {
    NOVAFIX_PLUGIN_RESULT_ACCEPTED = 0,
    NOVAFIX_PLUGIN_RESULT_UNSUPPORTED = 1,
    NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT = 2,
    NOVAFIX_PLUGIN_RESULT_BUSY = 3
} NovaFixPluginResult;

// Queues a UTF-8 native message. ACCEPTED means the message was queued,
// not that the player acknowledged it
typedef uint32_t(NOVAFIX_PLUGIN_CALL* NovaFixPluginShowNativeMessageFn)(
    const char* utf8Text);

typedef enum NovaFixPluginNativeMessageFlags {
    // Routes confirm/cancel to the native message instead of the game
    NOVAFIX_PLUGIN_NATIVE_MESSAGE_EXCLUSIVE_INPUT = 1u << 0
} NovaFixPluginNativeMessageFlags;

typedef struct NovaFixPluginNativeMessageOptions {
    uint32_t structSize;
    uint32_t flags;
    uint32_t delayMs;
    // Time before closing the message. Zero waits for player input
    uint32_t durationMs;
} NovaFixPluginNativeMessageOptions;

typedef uint32_t (NOVAFIX_PLUGIN_CALL *NovaFixPluginShowNativeMessageExFn)(
    const char* utf8Text,
    const NovaFixPluginNativeMessageOptions* options);

typedef float (NOVAFIX_PLUGIN_CALL *NovaFixPluginReadValueFn)(void* userData);
typedef void (NOVAFIX_PLUGIN_CALL *NovaFixPluginWriteValueFn)(
    void* userData, float value);
typedef void (NOVAFIX_PLUGIN_CALL *NovaFixPluginActivateControlFn)(
    void* userData);

// LR exposes these under the native Add-ons menu; XIII and XIII-2 ignore them
typedef enum NovaFixPluginNativeMenuItemType {
    NOVAFIX_PLUGIN_NATIVE_MENU_ACTION = 1,
    NOVAFIX_PLUGIN_NATIVE_MENU_CHECKBOX = 2,
    NOVAFIX_PLUGIN_NATIVE_MENU_RADIO = 3,
    NOVAFIX_PLUGIN_NATIVE_MENU_SEPARATOR = 4,
    NOVAFIX_PLUGIN_NATIVE_MENU_SUBMENU = 5
} NovaFixPluginNativeMenuItemType;

typedef enum NovaFixPluginNativeMenuState {
    NOVAFIX_PLUGIN_NATIVE_MENU_ENABLED = 1u << 0,
    NOVAFIX_PLUGIN_NATIVE_MENU_CHECKED = 1u << 1
} NovaFixPluginNativeMenuState;

typedef uint32_t (NOVAFIX_PLUGIN_CALL *NovaFixPluginReadMenuStateFn)(
    void* userData);
typedef void (NOVAFIX_PLUGIN_CALL *NovaFixPluginActivateMenuItemFn)(
    void* userData);

typedef struct NovaFixPluginNativeMenuItem {
    uint32_t structSize;
    uint32_t type;
    const char* id;
    const char* label;
    // -1 places the item at the root; otherwise indexes an earlier SUBMENU in this array
    int32_t parentIndex;
    NovaFixPluginReadMenuStateFn readState;
    NovaFixPluginActivateMenuItemFn activate;
    void* userData;
} NovaFixPluginNativeMenuItem;

typedef struct NovaFixPluginNativeMenu {
    uint32_t structSize;
    const char* id;
    const char* label;
    uint32_t itemCount;
    uint32_t itemStride;
    const NovaFixPluginNativeMenuItem* items;
} NovaFixPluginNativeMenu;

typedef struct NovaFixPluginControl {
    uint32_t structSize;
    uint32_t type;
    const char* id;
    const char* label;
    const char* description;
    float minimum;
    float maximum;
    float step;
    NovaFixPluginReadValueFn readValue;
    NovaFixPluginWriteValueFn writeValue;
    void* userData;
    // BUTTON uses activate; COMBO uses optionCount/options
    NovaFixPluginActivateControlFn activate;
    uint32_t optionCount;
    const char* const* options;
} NovaFixPluginControl;

typedef struct NovaFixPluginDescriptor {
    uint32_t structSize;
    uint32_t apiVersion;
    const char* id;
    const char* name;
    uint32_t controlCount;
    const NovaFixPluginControl* controls;
    // nativeMenuStride allows NovaFixPluginNativeMenu to grow without breaking ABI
    uint32_t nativeMenuCount;
    uint32_t nativeMenuStride;
    const NovaFixPluginNativeMenu* nativeMenus;
    // 0 uses this header's control size; older descriptors use the legacy stride
    uint32_t controlStride;
} NovaFixPluginDescriptor;

// The host data and strings remain valid until the game exits
typedef struct NovaFixPluginHost {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t game;
    const char* gameName;
    const char* novaFixVersion;
    // Optional LR services; check structSize and the pointer before use
    NovaFixPluginShowNativeMessageFn showNativeMessage;
    NovaFixPluginShowNativeMessageExFn showNativeMessageEx;
} NovaFixPluginHost;

static inline NovaFixPluginNativeMessageOptions
NovaFixPlugin_MakeNativeMessageOptions(void) {
    NovaFixPluginNativeMessageOptions options = {
        (uint32_t)sizeof(NovaFixPluginNativeMessageOptions),
        NOVAFIX_PLUGIN_NATIVE_MESSAGE_EXCLUSIVE_INPUT, 0u, 0u};
    return options;
}

typedef const NovaFixPluginDescriptor* (NOVAFIX_PLUGIN_CALL *
    NovaFixPluginQueryFn)(const NovaFixPluginHost* host);

static inline NovaFixPluginControl NovaFixPlugin_MakeCheckbox(
    const char* id, const char* label, const char* description,
    NovaFixPluginReadValueFn readValue, NovaFixPluginWriteValueFn writeValue,
    void* userData) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_CHECKBOX,
        id, label, description, 0.0f, 1.0f, 1.0f,
        readValue, writeValue, userData, (NovaFixPluginActivateControlFn)0,
        0u, (const char* const*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeIntSlider(
    const char* id, const char* label, const char* description,
    float minimum, float maximum, float step,
    NovaFixPluginReadValueFn readValue, NovaFixPluginWriteValueFn writeValue,
    void* userData) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_INT_SLIDER,
        id, label, description, minimum, maximum, step,
        readValue, writeValue, userData, (NovaFixPluginActivateControlFn)0,
        0u, (const char* const*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeFloatSlider(
    const char* id, const char* label, const char* description,
    float minimum, float maximum, float step,
    NovaFixPluginReadValueFn readValue, NovaFixPluginWriteValueFn writeValue,
    void* userData) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_FLOAT_SLIDER,
        id, label, description, minimum, maximum, step,
        readValue, writeValue, userData, (NovaFixPluginActivateControlFn)0,
        0u, (const char* const*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeButton(
    const char* id, const char* label, const char* description,
    NovaFixPluginActivateControlFn activate, void* userData) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_BUTTON,
        id, label, description, 0.0f, 0.0f, 0.0f,
        (NovaFixPluginReadValueFn)0, (NovaFixPluginWriteValueFn)0, userData,
        activate, 0u, (const char* const*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeCombo(
    const char* id, const char* label, const char* description,
    uint32_t optionCount, const char* const* options,
    NovaFixPluginReadValueFn readValue, NovaFixPluginWriteValueFn writeValue,
    void* userData) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_COMBO,
        id, label, description, 0.0f,
        optionCount ? (float)(optionCount - 1u) : 0.0f, 1.0f,
        readValue, writeValue, userData,
        (NovaFixPluginActivateControlFn)0, optionCount, options};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeHeader(
    const char* id, const char* label) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_HEADER,
        id, label, (const char*)0, 0.0f, 0.0f, 0.0f,
        (NovaFixPluginReadValueFn)0, (NovaFixPluginWriteValueFn)0,
        (void*)0, (NovaFixPluginActivateControlFn)0, 0u,
        (const char* const*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeSeparator(
    const char* id) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_SEPARATOR,
        id, (const char*)0, (const char*)0, 0.0f, 0.0f, 0.0f,
        (NovaFixPluginReadValueFn)0, (NovaFixPluginWriteValueFn)0,
        (void*)0, (NovaFixPluginActivateControlFn)0, 0u,
        (const char* const*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeText(
    const char* id, const char* text) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_TEXT,
        id, text, (const char*)0, 0.0f, 0.0f, 0.0f,
        (NovaFixPluginReadValueFn)0, (NovaFixPluginWriteValueFn)0,
        (void*)0, (NovaFixPluginActivateControlFn)0, 0u,
        (const char* const*)0};
    return control;
}

static inline NovaFixPluginDescriptor NovaFixPlugin_MakeDescriptor(
    const char* id, const char* name, uint32_t controlCount,
    const NovaFixPluginControl* controls) {
    NovaFixPluginDescriptor descriptor = {
        (uint32_t)sizeof(NovaFixPluginDescriptor), NOVAFIX_PLUGIN_API_VERSION,
        id, name, controlCount, controls, 0u,
        (uint32_t)sizeof(NovaFixPluginNativeMenu),
        (const NovaFixPluginNativeMenu*)0,
        (uint32_t)sizeof(NovaFixPluginControl)};
    return descriptor;
}

static inline NovaFixPluginNativeMenuItem NovaFixPlugin_MakeNativeMenuItem(
    uint32_t type, const char* id, const char* label, int32_t parentIndex,
    NovaFixPluginReadMenuStateFn readState,
    NovaFixPluginActivateMenuItemFn activate, void* userData) {
    NovaFixPluginNativeMenuItem item = {
        (uint32_t)sizeof(NovaFixPluginNativeMenuItem), type, id, label,
        parentIndex, readState, activate, userData};
    return item;
}

static inline NovaFixPluginNativeMenu NovaFixPlugin_MakeNativeMenu(
    const char* id, const char* label, uint32_t itemCount,
    const NovaFixPluginNativeMenuItem* items) {
    NovaFixPluginNativeMenu menu = {
        (uint32_t)sizeof(NovaFixPluginNativeMenu), id, label, itemCount,
        (uint32_t)sizeof(NovaFixPluginNativeMenuItem), items};
    return menu;
}

static inline void NovaFixPlugin_SetNativeMenus(
    NovaFixPluginDescriptor* descriptor, uint32_t menuCount,
    const NovaFixPluginNativeMenu* menus) {
    if (!descriptor) return;
    descriptor->nativeMenuCount = menuCount;
    descriptor->nativeMenuStride = (uint32_t)sizeof(NovaFixPluginNativeMenu);
    descriptor->nativeMenus = menus;
}
