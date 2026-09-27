#pragma once

#include <stddef.h>
#include <stdint.h>

#if UINTPTR_MAX != UINT32_MAX
#error NovaFix plugins must be built for 32-bit x86
#endif

#define NOVAFIX_PLUGIN_ABI_VERSION 1u
#define NOVAFIX_PLUGIN_API_VERSION NOVAFIX_PLUGIN_ABI_VERSION
#define NOVAFIX_PLUGIN_QUERY_EXPORT "NovaFixPlugin_Query"

#define NOVAFIX_PLUGIN_SERVICE_LOG "novafix.log"
#define NOVAFIX_PLUGIN_SERVICE_NATIVE_NOTIFICATIONS \
    "novafix.native_notifications"
#define NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION 1u
#define NOVAFIX_PLUGIN_LOG_SERVICE_VERSION 1u
#define NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_SERVICE_VERSION 1u

#define NOVAFIX_PLUGIN_GLYPH_CONFIRM "{button:confirm}"
#define NOVAFIX_PLUGIN_GLYPH_CANCEL "{button:cancel}"
#define NOVAFIX_PLUGIN_GLYPH_FACE_LEFT "{button:face-left}"
#define NOVAFIX_PLUGIN_GLYPH_FACE_TOP "{button:face-top}"
#define NOVAFIX_PLUGIN_GLYPH_MENU "{button:menu}"

// UTF-8 byte limits exclude the null terminator. Future hosts may raise them.
#define NOVAFIX_PLUGIN_MAX_ID_BYTES 64u
#define NOVAFIX_PLUGIN_MAX_LABEL_BYTES 128u
#define NOVAFIX_PLUGIN_MAX_DESCRIPTION_BYTES 1024u
#define NOVAFIX_PLUGIN_MAX_VALUE_AFFIX_BYTES 24u
#define NOVAFIX_PLUGIN_MAX_STATUS_TEXT_BYTES 2048u
#define NOVAFIX_PLUGIN_MAX_LOG_MESSAGE_BYTES 900u
#define NOVAFIX_PLUGIN_MAX_NATIVE_MESSAGE_BYTES 2048u
#define NOVAFIX_PLUGIN_MAX_NATIVE_MESSAGE_QUEUE 8u
#define NOVAFIX_PLUGIN_MAX_NATIVE_MESSAGE_TIME_MS 86400000u
#define NOVAFIX_PLUGIN_MAX_CONTROLS 256u
#define NOVAFIX_PLUGIN_MAX_COMBO_OPTIONS 256u
#define NOVAFIX_PLUGIN_MAX_RADIO_OPTIONS 12u
#define NOVAFIX_PLUGIN_MAX_NATIVE_MENUS 32u
#define NOVAFIX_PLUGIN_MAX_NATIVE_MENU_ITEMS 512u
#define NOVAFIX_PLUGIN_MAX_CONTROL_EXTENSIONS 16u

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
    NOVAFIX_PLUGIN_CONTROL_TEXT = 8,
    NOVAFIX_PLUGIN_CONTROL_STATUS_TEXT = 9,
    NOVAFIX_PLUGIN_CONTROL_COLOR = 10,
    NOVAFIX_PLUGIN_CONTROL_SECTION = 11,
    NOVAFIX_PLUGIN_CONTROL_RADIO_GROUP = 12
} NovaFixPluginControlType;

typedef enum NovaFixPluginExtensionType {
    NOVAFIX_PLUGIN_EXTENSION_CONTROL_STATE = 0x1001,
    NOVAFIX_PLUGIN_EXTENSION_STATUS_TEXT = 0x1002,
    NOVAFIX_PLUGIN_EXTENSION_COLOR = 0x1003,
    NOVAFIX_PLUGIN_EXTENSION_PRESENTATION = 0x1004,
    NOVAFIX_PLUGIN_EXTENSION_VALUE_DISPLAY = 0x1005
} NovaFixPluginExtensionType;

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

typedef enum NovaFixPluginLogLevel {
    NOVAFIX_PLUGIN_LOG_DEBUG = 1,
    NOVAFIX_PLUGIN_LOG_INFO = 2,
    NOVAFIX_PLUGIN_LOG_WARNING = 3,
    NOVAFIX_PLUGIN_LOG_ERROR = 4
} NovaFixPluginLogLevel;

typedef struct NovaFixPluginExtension {
    uint32_t structSize;
    uint32_t type;
    uint32_t version;
    const struct NovaFixPluginExtension* next;
} NovaFixPluginExtension;

typedef enum NovaFixPluginControlState {
    NOVAFIX_PLUGIN_CONTROL_STATE_VISIBLE = 1u << 0,
    NOVAFIX_PLUGIN_CONTROL_STATE_ENABLED = 1u << 1,
    NOVAFIX_PLUGIN_CONTROL_STATE_READ_ONLY = 1u << 2,
    NOVAFIX_PLUGIN_CONTROL_STATE_BUSY = 1u << 3
} NovaFixPluginControlState;

typedef struct NovaFixPluginColor {
    float red;
    float green;
    float blue;
    float alpha;
} NovaFixPluginColor;

typedef enum NovaFixPluginColorFlags {
    NOVAFIX_PLUGIN_COLOR_ALPHA = 1u << 0
} NovaFixPluginColorFlags;

typedef enum NovaFixPluginPresentationFlags {
    NOVAFIX_PLUGIN_PRESENTATION_ADVANCED = 1u << 0,
    NOVAFIX_PLUGIN_PRESENTATION_RESTART_REQUIRED = 1u << 1,
    NOVAFIX_PLUGIN_PRESENTATION_EXPERIMENTAL = 1u << 2,
    // Applies to SECTION controls only
    NOVAFIX_PLUGIN_PRESENTATION_COLLAPSED_BY_DEFAULT = 1u << 3
} NovaFixPluginPresentationFlags;

typedef enum NovaFixPluginHeaderColorFlags {
    NOVAFIX_PLUGIN_HEADER_COLOR_APPLY_TO_CONTENT = 1u << 0
} NovaFixPluginHeaderColorFlags;

#define NOVAFIX_PLUGIN_DECIMALS_DEFAULT 0xFFFFFFFFu

typedef uint32_t (NOVAFIX_PLUGIN_CALL *NovaFixPluginReadControlStateFn)(
    void* userData);
typedef uint32_t (NOVAFIX_PLUGIN_CALL *NovaFixPluginReadTextFn)(
    void* userData, char* output, uint32_t capacity);
typedef void (NOVAFIX_PLUGIN_CALL *NovaFixPluginReadColorFn)(
    void* userData, NovaFixPluginColor* output);
typedef void (NOVAFIX_PLUGIN_CALL *NovaFixPluginWriteColorFn)(
    void* userData, const NovaFixPluginColor* value);

typedef struct NovaFixPluginControlStateExtension {
    NovaFixPluginExtension header;
    NovaFixPluginReadControlStateFn readState;
    void* userData;
} NovaFixPluginControlStateExtension;

typedef struct NovaFixPluginStatusTextExtension {
    NovaFixPluginExtension header;
    NovaFixPluginReadTextFn readText;
    void* userData;
    uint32_t refreshIntervalMs;
} NovaFixPluginStatusTextExtension;

typedef struct NovaFixPluginColorExtension {
    NovaFixPluginExtension header;
    NovaFixPluginReadColorFn readColor;
    NovaFixPluginWriteColorFn writeColor;
    void* userData;
    uint32_t flags;
} NovaFixPluginColorExtension;

typedef struct NovaFixPluginPresentationExtension {
    NovaFixPluginExtension header;
    uint32_t flags;
} NovaFixPluginPresentationExtension;

typedef struct NovaFixPluginValueDisplayExtension {
    NovaFixPluginExtension header;
    const char* prefix;
    const char* suffix;
    // 0-6, or NOVAFIX_PLUGIN_DECIMALS_DEFAULT
    uint32_t decimalPlaces;
} NovaFixPluginValueDisplayExtension;

typedef struct NovaFixPluginServiceHeader {
    uint32_t structSize;
    uint32_t version;
} NovaFixPluginServiceHeader;

typedef const void* (NOVAFIX_PLUGIN_CALL *NovaFixPluginGetServiceFn)(
    void* hostContext, const char* serviceId, uint32_t minimumVersion);

typedef uint32_t (NOVAFIX_PLUGIN_CALL *NovaFixPluginLogWriteFn)(
    void* serviceContext, uint32_t level, const char* pluginId,
    const char* utf8Message);

typedef struct NovaFixPluginLogService {
    NovaFixPluginServiceHeader header;
    void* serviceContext;
    NovaFixPluginLogWriteFn write;
} NovaFixPluginLogService;

typedef enum NovaFixPluginNativeNotificationMode {
    // Transient message; durationMs must be non-zero.
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_TIMED = 1,
    // Modal message that captures confirm/cancel and blocks gameplay input.
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_ACKNOWLEDGE = 2
} NovaFixPluginNativeNotificationMode;

typedef enum NovaFixPluginNativeNotificationCapability {
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_TIMED = 1u << 0,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_ACKNOWLEDGE = 1u << 1,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_ACKNOWLEDGE_BLOCKS_GAME_INPUT =
        1u << 2,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_POSITION_GRID = 1u << 3,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_CAP_INLINE_GLYPHS = 1u << 4
} NovaFixPluginNativeNotificationCapability;

typedef enum NovaFixPluginNativeNotificationPosition {
    // Preserves the host default, currently screen center.
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_DEFAULT = 0,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_TOP_LEFT = 1,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_TOP_CENTER = 2,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_TOP_RIGHT = 3,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_CENTER_LEFT = 4,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_CENTER = 5,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_CENTER_RIGHT = 6,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_BOTTOM_LEFT = 7,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_BOTTOM_CENTER = 8,
    NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_BOTTOM_RIGHT = 9
} NovaFixPluginNativeNotificationPosition;

typedef struct NovaFixPluginNativeNotificationOptions {
    uint32_t structSize;
    uint32_t mode;
    // Reserved for future use. Must be zero.
    uint32_t flags;
    uint32_t delayMs;
    // Required for TIMED; must be zero for ACKNOWLEDGE.
    uint32_t durationMs;
    // Optional 3x3 screen anchor. Older hosts ignore this trailing field.
    uint32_t position;
} NovaFixPluginNativeNotificationOptions;

typedef uint32_t (NOVAFIX_PLUGIN_CALL *
    NovaFixPluginEnqueueNativeNotificationFn)(
        void* serviceContext, const char* utf8Text,
        const NovaFixPluginNativeNotificationOptions* options);

typedef struct NovaFixPluginNativeNotificationService {
    NovaFixPluginServiceHeader header;
    void* serviceContext;
    uint32_t capabilities;
    NovaFixPluginEnqueueNativeNotificationFn enqueue;
} NovaFixPluginNativeNotificationService;

// Legacy queue wrapper; ACCEPTED does not mean player acknowledgement.
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
    // -1 selects the root; otherwise indexes an earlier SUBMENU.
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
    // BUTTON uses activate; COMBO and RADIO_GROUP use optionCount/options
    NovaFixPluginActivateControlFn activate;
    uint32_t optionCount;
    const char* const* options;
    const NovaFixPluginExtension* extensions;
} NovaFixPluginControl;

typedef struct NovaFixPluginDescriptor {
    uint32_t structSize;
    uint32_t apiVersion;
    const char* id;
    const char* name;
    uint32_t controlCount;
    const NovaFixPluginControl* controls;
    // Element stride for forward-compatible ABI growth.
    uint32_t nativeMenuCount;
    uint32_t nativeMenuStride;
    const NovaFixPluginNativeMenu* nativeMenus;
    // Zero selects the legacy 56-byte control layout.
    uint32_t controlStride;
    const char* author;
    const char* version;
    const char* description;
    const char* website;
    // Alpha zero keeps the default header color.
    NovaFixPluginColor headerColor;
    uint32_t headerColorFlags;
} NovaFixPluginDescriptor;

// The host data and strings remain valid until the game exits
typedef struct NovaFixPluginHost {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t game;
    const char* gameName;
    const char* novaFixVersion;
    // Optional LR services; validate structSize and the pointer.
    NovaFixPluginShowNativeMessageFn showNativeMessage;
    NovaFixPluginShowNativeMessageExFn showNativeMessageEx;
    // Optional independently versioned service registry.
    void* hostContext;
    NovaFixPluginGetServiceFn getService;
} NovaFixPluginHost;

static inline uint32_t NovaFixPlugin_ShowNativeMessage(
    const NovaFixPluginHost* host, const char* utf8Text) {
    const size_t required = offsetof(NovaFixPluginHost, showNativeMessage) +
                            sizeof(host->showNativeMessage);
    if (!host || host->structSize < required || !host->showNativeMessage) {
        return NOVAFIX_PLUGIN_RESULT_UNSUPPORTED;
    }
    return host->showNativeMessage(utf8Text);
}

static inline uint32_t NovaFixPlugin_ShowNativeMessageEx(
    const NovaFixPluginHost* host, const char* utf8Text,
    const NovaFixPluginNativeMessageOptions* options) {
    const size_t required = offsetof(NovaFixPluginHost, showNativeMessageEx) +
                            sizeof(host->showNativeMessageEx);
    if (!host || host->structSize < required || !host->showNativeMessageEx) {
        return NOVAFIX_PLUGIN_RESULT_UNSUPPORTED;
    }
    return host->showNativeMessageEx(utf8Text, options);
}

static inline const void* NovaFixPlugin_GetService(
    const NovaFixPluginHost* host, const char* serviceId,
    uint32_t minimumVersion) {
    const size_t required = offsetof(NovaFixPluginHost, getService) +
                            sizeof(host->getService);
    if (!host || host->structSize < required || !host->getService ||
        !serviceId) {
        return (const void*)0;
    }
    return host->getService(host->hostContext, serviceId, minimumVersion);
}

static inline const NovaFixPluginLogService* NovaFixPlugin_GetLogService(
    const NovaFixPluginHost* host) {
    return (const NovaFixPluginLogService*)NovaFixPlugin_GetService(
        host, NOVAFIX_PLUGIN_SERVICE_LOG,
        NOVAFIX_PLUGIN_LOG_SERVICE_VERSION);
}

static inline const NovaFixPluginNativeNotificationService*
NovaFixPlugin_GetNativeNotificationService(
    const NovaFixPluginHost* host) {
    return (const NovaFixPluginNativeNotificationService*)
        NovaFixPlugin_GetService(
            host, NOVAFIX_PLUGIN_SERVICE_NATIVE_NOTIFICATIONS,
            NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_SERVICE_VERSION);
}

static inline uint32_t NovaFixPlugin_GetNativeNotificationCapabilities(
    const NovaFixPluginHost* host) {
    const NovaFixPluginNativeNotificationService* service =
        NovaFixPlugin_GetNativeNotificationService(host);
    const size_t required =
        offsetof(NovaFixPluginNativeNotificationService, capabilities) +
        sizeof(service->capabilities);
    return service && service->header.structSize >= required
        ? service->capabilities
        : 0u;
}

static inline uint32_t NovaFixPlugin_Notify(
    const NovaFixPluginHost* host, const char* utf8Text,
    const NovaFixPluginNativeNotificationOptions* options) {
    const NovaFixPluginNativeNotificationService* service =
        NovaFixPlugin_GetNativeNotificationService(host);
    const size_t required =
        offsetof(NovaFixPluginNativeNotificationService, enqueue) +
        sizeof(service->enqueue);
    if (!service || service->header.structSize < required ||
        service->header.version <
            NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_SERVICE_VERSION ||
        !service->enqueue) {
        return NOVAFIX_PLUGIN_RESULT_UNSUPPORTED;
    }
    return service->enqueue(service->serviceContext, utf8Text, options);
}

static inline uint32_t NovaFixPlugin_Log(
    const NovaFixPluginHost* host, uint32_t level, const char* pluginId,
    const char* utf8Message) {
    const NovaFixPluginLogService* service =
        NovaFixPlugin_GetLogService(host);
    const size_t required = offsetof(NovaFixPluginLogService, write) +
                            sizeof(service->write);
    if (!service ||
        service->header.structSize < required ||
        service->header.version < NOVAFIX_PLUGIN_LOG_SERVICE_VERSION ||
        !service->write) {
        return NOVAFIX_PLUGIN_RESULT_UNSUPPORTED;
    }
    return service->write(service->serviceContext, level, pluginId,
                          utf8Message);
}

static inline NovaFixPluginNativeMessageOptions
NovaFixPlugin_MakeNativeMessageOptions(void) {
    NovaFixPluginNativeMessageOptions options = {
        (uint32_t)sizeof(NovaFixPluginNativeMessageOptions),
        NOVAFIX_PLUGIN_NATIVE_MESSAGE_EXCLUSIVE_INPUT, 0u, 0u};
    return options;
}

static inline NovaFixPluginNativeNotificationOptions
NovaFixPlugin_MakeTimedNotificationOptions(uint32_t durationMs) {
    NovaFixPluginNativeNotificationOptions options = {
        (uint32_t)sizeof(NovaFixPluginNativeNotificationOptions),
        NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_TIMED, 0u, 0u, durationMs,
        NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_DEFAULT};
    return options;
}

static inline NovaFixPluginNativeNotificationOptions
NovaFixPlugin_MakeAcknowledgementNotificationOptions(void) {
    NovaFixPluginNativeNotificationOptions options = {
        (uint32_t)sizeof(NovaFixPluginNativeNotificationOptions),
        NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_ACKNOWLEDGE, 0u, 0u, 0u,
        NOVAFIX_PLUGIN_NATIVE_NOTIFICATION_POSITION_DEFAULT};
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
        0u, (const char* const*)0, (const NovaFixPluginExtension*)0};
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
        0u, (const char* const*)0, (const NovaFixPluginExtension*)0};
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
        0u, (const char* const*)0, (const NovaFixPluginExtension*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeButton(
    const char* id, const char* label, const char* description,
    NovaFixPluginActivateControlFn activate, void* userData) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_BUTTON,
        id, label, description, 0.0f, 0.0f, 0.0f,
        (NovaFixPluginReadValueFn)0, (NovaFixPluginWriteValueFn)0, userData,
        activate, 0u, (const char* const*)0,
        (const NovaFixPluginExtension*)0};
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
        (NovaFixPluginActivateControlFn)0, optionCount, options,
        (const NovaFixPluginExtension*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeRadioGroup(
    const char* id, const char* label, const char* description,
    uint32_t optionCount, const char* const* options,
    NovaFixPluginReadValueFn readValue, NovaFixPluginWriteValueFn writeValue,
    void* userData) {
    NovaFixPluginControl control = NovaFixPlugin_MakeCombo(
        id, label, description, optionCount, options,
        readValue, writeValue, userData);
    control.type = NOVAFIX_PLUGIN_CONTROL_RADIO_GROUP;
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeHeader(
    const char* id, const char* label) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_HEADER,
        id, label, (const char*)0, 0.0f, 0.0f, 0.0f,
        (NovaFixPluginReadValueFn)0, (NovaFixPluginWriteValueFn)0,
        (void*)0, (NovaFixPluginActivateControlFn)0, 0u,
        (const char* const*)0, (const NovaFixPluginExtension*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeSeparator(
    const char* id) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_SEPARATOR,
        id, (const char*)0, (const char*)0, 0.0f, 0.0f, 0.0f,
        (NovaFixPluginReadValueFn)0, (NovaFixPluginWriteValueFn)0,
        (void*)0, (NovaFixPluginActivateControlFn)0, 0u,
        (const char* const*)0, (const NovaFixPluginExtension*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeText(
    const char* id, const char* text) {
    NovaFixPluginControl control = {
        (uint32_t)sizeof(NovaFixPluginControl), NOVAFIX_PLUGIN_CONTROL_TEXT,
        id, text, (const char*)0, 0.0f, 0.0f, 0.0f,
        (NovaFixPluginReadValueFn)0, (NovaFixPluginWriteValueFn)0,
        (void*)0, (NovaFixPluginActivateControlFn)0, 0u,
        (const char* const*)0, (const NovaFixPluginExtension*)0};
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeSection(
    const char* id, const char* label, const char* description) {
    NovaFixPluginControl control = NovaFixPlugin_MakeText(id, label);
    control.type = NOVAFIX_PLUGIN_CONTROL_SECTION;
    control.description = description;
    return control;
}

static inline NovaFixPluginControlStateExtension
NovaFixPlugin_MakeControlStateExtension(
    NovaFixPluginReadControlStateFn readState, void* userData) {
    NovaFixPluginControlStateExtension extension = {
        {(uint32_t)sizeof(NovaFixPluginControlStateExtension),
         NOVAFIX_PLUGIN_EXTENSION_CONTROL_STATE,
         NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION,
         (const NovaFixPluginExtension*)0},
        readState, userData};
    return extension;
}

static inline NovaFixPluginStatusTextExtension
NovaFixPlugin_MakeStatusTextExtension(
    NovaFixPluginReadTextFn readText, void* userData,
    uint32_t refreshIntervalMs) {
    NovaFixPluginStatusTextExtension extension = {
        {(uint32_t)sizeof(NovaFixPluginStatusTextExtension),
         NOVAFIX_PLUGIN_EXTENSION_STATUS_TEXT,
         NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION,
         (const NovaFixPluginExtension*)0},
        readText, userData, refreshIntervalMs};
    return extension;
}

static inline NovaFixPluginColorExtension NovaFixPlugin_MakeColorExtension(
    NovaFixPluginReadColorFn readColor, NovaFixPluginWriteColorFn writeColor,
    void* userData, uint32_t flags) {
    NovaFixPluginColorExtension extension = {
        {(uint32_t)sizeof(NovaFixPluginColorExtension),
         NOVAFIX_PLUGIN_EXTENSION_COLOR,
         NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION,
         (const NovaFixPluginExtension*)0},
        readColor, writeColor, userData, flags};
    return extension;
}

static inline NovaFixPluginPresentationExtension
NovaFixPlugin_MakePresentationExtension(uint32_t flags) {
    NovaFixPluginPresentationExtension extension = {
        {(uint32_t)sizeof(NovaFixPluginPresentationExtension),
         NOVAFIX_PLUGIN_EXTENSION_PRESENTATION,
         NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION,
         (const NovaFixPluginExtension*)0},
        flags};
    return extension;
}

static inline NovaFixPluginValueDisplayExtension
NovaFixPlugin_MakeValueDisplayExtension(
    const char* prefix, const char* suffix, uint32_t decimalPlaces) {
    NovaFixPluginValueDisplayExtension extension = {
        {(uint32_t)sizeof(NovaFixPluginValueDisplayExtension),
         NOVAFIX_PLUGIN_EXTENSION_VALUE_DISPLAY,
         NOVAFIX_PLUGIN_CONTROL_EXTENSION_VERSION,
         (const NovaFixPluginExtension*)0},
        prefix, suffix, decimalPlaces};
    return extension;
}

static inline void NovaFixPlugin_AddControlExtension(
    NovaFixPluginControl* control, NovaFixPluginExtension* extension) {
    const NovaFixPluginExtension* current;
    uint32_t count;
    if (!control || !extension) return;
    current = control->extensions;
    for (count = 0u; current && count < NOVAFIX_PLUGIN_MAX_CONTROL_EXTENSIONS;
         ++count, current = current->next) {
        if (current == extension) return;
    }
    if (current || count == NOVAFIX_PLUGIN_MAX_CONTROL_EXTENSIONS) return;
    extension->next = control->extensions;
    control->extensions = extension;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeStatusText(
    const char* id, const char* label, const char* description,
    NovaFixPluginStatusTextExtension* extension) {
    NovaFixPluginControl control = NovaFixPlugin_MakeText(id, label);
    control.type = NOVAFIX_PLUGIN_CONTROL_STATUS_TEXT;
    control.description = description;
    if (extension) {
        NovaFixPlugin_AddControlExtension(&control, &extension->header);
    }
    return control;
}

static inline NovaFixPluginControl NovaFixPlugin_MakeColor(
    const char* id, const char* label, const char* description,
    NovaFixPluginColorExtension* extension) {
    NovaFixPluginControl control = NovaFixPlugin_MakeText(id, label);
    control.type = NOVAFIX_PLUGIN_CONTROL_COLOR;
    control.description = description;
    if (extension) {
        NovaFixPlugin_AddControlExtension(&control, &extension->header);
    }
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
        (uint32_t)sizeof(NovaFixPluginControl),
        (const char*)0, (const char*)0, (const char*)0, (const char*)0,
        {0.0f, 0.0f, 0.0f, 0.0f}, 0u};
    return descriptor;
}

static inline void NovaFixPlugin_SetMetadata(
    NovaFixPluginDescriptor* descriptor, const char* author,
    const char* version, const char* description, const char* website) {
    if (!descriptor) return;
    descriptor->author = author;
    descriptor->version = version;
    descriptor->description = description;
    descriptor->website = website;
}

static inline void NovaFixPlugin_SetHeaderColor(
    NovaFixPluginDescriptor* descriptor, NovaFixPluginColor color,
    uint32_t flags) {
    if (!descriptor) return;
    descriptor->headerColor = color;
    descriptor->headerColorFlags = flags;
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

#if defined(__cplusplus)
#define NOVAFIX_PLUGIN_STATIC_ASSERT(condition) static_assert(condition)
#else
#define NOVAFIX_PLUGIN_STATIC_ASSERT(condition) _Static_assert(condition, #condition)
#endif

NOVAFIX_PLUGIN_STATIC_ASSERT(sizeof(void*) == 4u);
NOVAFIX_PLUGIN_STATIC_ASSERT(
    offsetof(NovaFixPluginControl, extensions) == 56u);
NOVAFIX_PLUGIN_STATIC_ASSERT(
    offsetof(NovaFixPluginDescriptor, author) == 40u);
NOVAFIX_PLUGIN_STATIC_ASSERT(
    offsetof(NovaFixPluginHost, hostContext) == 28u);

#undef NOVAFIX_PLUGIN_STATIC_ASSERT
