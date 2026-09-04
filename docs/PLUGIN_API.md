The API lets add-ons provide:

- controls in the Add-ons tab
- entries in LR's Add-ons menu
- LR message windows

> [!NOTE]
> The API is optional; regular x86 DLL and ASI mods load without it

[`include/novafix/plugin_api.h`](../include/novafix/plugin_api.h)



## Query

Export:

```cpp
NOVAFIX_PLUGIN_EXPORT const NovaFixPluginDescriptor* NOVAFIX_PLUGIN_CALL
NovaFixPlugin_Query(const NovaFixPluginHost* host);
```

The query runs after `DllMain`. Return `nullptr` to skip API registration

Example:

```cpp
#include <novafix/plugin_api.h>

namespace {
float g_enabled = 1.0f;

float NOVAFIX_PLUGIN_CALL ReadEnabled(void*) {
    return g_enabled;
}

void NOVAFIX_PLUGIN_CALL WriteEnabled(void*, float value) {
    g_enabled = value;
}

NovaFixPluginControl g_controls[] = {
    NovaFixPlugin_MakeCheckbox(
        "enabled", "Enabled", nullptr,
        ReadEnabled, WriteEnabled, nullptr),
};

NovaFixPluginDescriptor g_descriptor = NovaFixPlugin_MakeDescriptor(
    "author.addon", "My Add-on", 1, g_controls);
} // namespace

NOVAFIX_PLUGIN_EXPORT const NovaFixPluginDescriptor* NOVAFIX_PLUGIN_CALL
NovaFixPlugin_Query(const NovaFixPluginHost* host) {
    if (!host || host->apiVersion != NOVAFIX_PLUGIN_API_VERSION ||
        host->structSize < offsetof(NovaFixPluginHost, showNativeMessage)) {
        return nullptr;
    }
    return &g_descriptor;
}
```

> Do not pass C++ exceptions across API callbacks






## Host

Field|Value
-|-
`apiVersion`|API version
`game`|`FF13`, `FF13_2`, `LIGHTNING_RETURNS`, or `UNKNOWN`
`gameName`|Display name; use `game` for detection
`novaFixVersion`|NovaFix version
`showNativeMessage`|Optional LR message function
`showNativeMessageEx`|Optional LR message function with timing

Check `structSize` and the function pointer before using optional fields
New services are appended to the struct






## Controls

Helper|Control
-|-
`NovaFixPlugin_MakeCheckbox`|Boolean value (`0.0f` or `1.0f`)
`NovaFixPlugin_MakeIntSlider`|Whole-number value
`NovaFixPlugin_MakeFloatSlider`|Floating-point value
`NovaFixPlugin_MakeCombo`|Zero-based option index
`NovaFixPlugin_MakeButton`|Action callback
`NovaFixPlugin_MakeHeader`|Section heading
`NovaFixPlugin_MakeSeparator`|Separator
`NovaFixPlugin_MakeText`|Wrapped text

`readValue` is polled while the Add-ons tab is visible. `writeValue` runs after an edit.
The add-on is responsible for applying and saving the value.
Both run on the render thread, so keep them short and do not call ImGui.

Limits:

- up to 256 controls per add-on
- control IDs must be non-empty and unique
- labels and descriptions are UTF-8
- descriptor text and combo options are copied during the query
- callbacks and `userData` must remain valid until process exit

> [!NOTE]
> Controls are declarative. The API does not expose an ImGui context or render callback





## LR menu

Item types: `ACTION`, `CHECKBOX`, `RADIO`, `SEPARATOR`, and
`SUBMENU`.

```cpp
uint32_t NOVAFIX_PLUGIN_CALL ReadMenuState(void*) {
    return NOVAFIX_PLUGIN_NATIVE_MENU_ENABLED |
           (g_enabled ? NOVAFIX_PLUGIN_NATIVE_MENU_CHECKED : 0u);
}

void NOVAFIX_PLUGIN_CALL Toggle(void*) {
    g_enabled = !g_enabled;
}

NovaFixPluginNativeMenuItem g_items[] = {
    NovaFixPlugin_MakeNativeMenuItem(
        NOVAFIX_PLUGIN_NATIVE_MENU_CHECKBOX,
        "enabled", "Enabled", -1,
        ReadMenuState, Toggle, nullptr),
};

NovaFixPluginNativeMenu g_menus[] = {
    NovaFixPlugin_MakeNativeMenu(
        "author.addon.menu", "My Add-on", 1, g_items),
};

// Inside NovaFixPlugin_Query:
if (host->game == NOVAFIX_PLUGIN_GAME_LIGHTNING_RETURNS) {
    NovaFixPlugin_SetNativeMenus(&g_descriptor, 1, g_menus);
}
```

`parentIndex = -1` places an item at the root. A child uses the index of an
earlier `SUBMENU` item. `readState` returns
`NOVAFIX_PLUGIN_NATIVE_MENU_ENABLED` and optionally
`NOVAFIX_PLUGIN_NATIVE_MENU_CHECKED`.

Menu callbacks run on LR's window thread. XIII and XIII-2 ignore native menu descriptors






## LR messages

`showNativeMessage` shows a native game message. `showNativeMessageEx` adds a delay and automatic close:

```cpp
NovaFixPluginNativeMessageOptions options =
    NovaFixPlugin_MakeNativeMessageOptions();
options.delayMs = 1000;
options.durationMs = 4000;

if (host->structSize >=
        offsetof(NovaFixPluginHost, showNativeMessageEx) +
            sizeof(host->showNativeMessageEx) &&
    host->showNativeMessageEx) {
    host->showNativeMessageEx("Saved successfully.", &options);
}
```

`durationMs = 0` waits for player input.
`NOVAFIX_PLUGIN_NATIVE_MESSAGE_EXCLUSIVE_INPUT` is required. Calls are
asynchronous; `ACCEPTED` only means the message was queued.

Result|Meaning
-|-
`NOVAFIX_PLUGIN_RESULT_ACCEPTED`|Queued
`NOVAFIX_PLUGIN_RESULT_UNSUPPORTED`|Unavailable for this game or executable
`NOVAFIX_PLUGIN_RESULT_INVALID_ARGUMENT`|Invalid text, options, flags, or timing
`NOVAFIX_PLUGIN_RESULT_BUSY`|Queue full

Limit|Value
-|-
Game|LR
Executable|Exact supported profile
Text|2048 UTF-8 bytes
Queue|8 messages
Delay/duration|24 hours

Messages are shown one at a time



> [!NOTE]
> Message functions are unavailable in XIII and XIII-2



> See [HOOKS_AND_PATCHES.md](HOOKS_AND_PATCHES.md) for hooks and executable patch locations.