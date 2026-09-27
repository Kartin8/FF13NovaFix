# NovaFix Plugin API

First of all - regular x86 `.dll` and `.asi` add-ons can be loaded without using the plugin API at all

The API is there for add-ons that want deeper integration: settings in the **Add-ons** tab, metadata, shared logging, native game notifications, or menu entries in Lightning Returns

For C++, use [`plugin.hpp`](../include/novafix/plugin.hpp). It provides a small C++17 wrapper around the stable C API in [`plugin_api.h`](../include/novafix/plugin_api.h)

Plugins are ONLY 32-bit x86

## Quick start

```cpp
#include <novafix/plugin.hpp>

namespace {
bool g_enabled = true;

float NOVAFIX_PLUGIN_CALL ReadEnabled(void*) {
    return g_enabled ? 1.0f : 0.0f;
}

void NOVAFIX_PLUGIN_CALL WriteEnabled(void*, float value) {
    g_enabled = value >= 0.5f;
}

novafix::Control g_controls[]{
    novafix::Control::checkbox(
        "enabled",
        "Enabled",
        "Turns the feature on or off.",
        ReadEnabled,
        WriteEnabled),
};

novafix::Plugin g_plugin(
    "author.my-addon",
    "My Add-on",
    g_controls,
    {
        "Author",
        "1.0.0",
        "Short description.",
        "https://example.com/my-addon"
    });
}

NOVAFIX_PLUGIN_IMPLEMENT(g_plugin)
```

Keep the `Control` array, `Plugin`, callbacks and callback `userData` alive for as long as the plugin is loaded, file-scope objects are usually the easiest way to do that

## Controls

The Add-ons tab currently supports:

Factory|What it does
-|-
`checkbox`|Boolean option
`intSlider`|Integer slider
`floatSlider`|Floating-point slider
`combo`|Drop-down list
`radioGroup`|Small visible choice list
`button`|Action button
`statusText`|Text that can refresh periodically
`color`|RGB/RGBA color picker
`section`|Collapsible settings group
`header`|Static heading
`separator`|Separator
`text`|Static text

Checkboxes use `0.0f` and `1.0f`, integer sliders still use float callbacks internally and the host rounds the value before passing it back. `combo` and `radioGroup` use zero-based indices

Radio groups support between 2 and `NOVAFIX_PLUGIN_MAX_RADIO_OPTIONS` entries, for anything longer just use a combo. Sections are flat, a section owns everything after it until the next section starts and nested sections aren't supported right now

Callbacks used by Add-ons tab controls run on the render thread while the tab is visible, so keep them lightweight and don't call ImGui directly from them. If a callback blows up, the host disables that callback instead of taking the whole plugin down with it

### Control state and presentation

Controls can provide a state callback using:

- `VISIBLE`
- `ENABLED`
- `READ_ONLY`
- `BUSY`

Most interactive controls just return:

```cpp
VISIBLE | ENABLED
```

`READ_ONLY` and `BUSY` both block editing

Presentation flags are:

- `ADVANCED`
- `RESTART_REQUIRED`
- `EXPERIMENTAL`
- `COLLAPSED_BY_DEFAULT` for sections

Example:

```cpp
novafix::Control::intSlider(
    "enemy-hp",
    "Enemy HP",
    nullptr,
    25.0f,
    500.0f,
    5.0f,
    ReadInt,
    WriteInt,
    &g_enemyHp,
    novafix::ControlOptions{}
        .state(ReadState)
        .presentation(NOVAFIX_PLUGIN_PRESENTATION_ADVANCED)
        .valueDisplay(nullptr, "%", 0u))
```

`valueDisplay(prefix, suffix, decimals)` controls how slider values are shown without exposing a raw `printf` format string to the host

`statusText` writes UTF-8 text into host-owned memory and returns the number of bytes written, not counting the null terminator. A refresh interval of `0` means it refreshes every visible frame

Color components use the `0.0f .. 1.0f` range

For most plugins the regular `Control` helpers are enough. If you need something lower-level, `Control::raw` and mutable `Control::descriptor()` expose the descriptor directly. `Control` itself is non-copyable and non-movable because it owns extension records referenced by its descriptor

## Plugin identity and metadata

Every plugin has a descriptor ID:

```text
author.my-addon
```

Treat this as a permanent ID, it's used for duplicate detection and saved UI state and comparison is case-insensitive

Changing the display name, author, version, description or website is fine. Changing the descriptor ID between releases isn't

Control IDs must be unique inside a plugin. Native menu IDs and native menu item IDs must also be unique inside their own scopes

Public text is UTF-8 and the `NOVAFIX_PLUGIN_MAX_*` constants define the maximum byte length for public strings, excluding the null terminator

`PluginMetadata` can also provide one custom header color. Alpha `0` keeps the default theme, and `NOVAFIX_PLUGIN_HEADER_COLOR_APPLY_TO_CONTENT` applies the same color to sections, frames, buttons, sliders and check marks. Plugins currently get one content accent color rather than separate colors for individual controls

## Host services

Plugins can write into `NovaFix.log`:

```cpp
g_plugin.info("Hello World");
g_plugin.warning("Help");
```

The logging service is thread-safe and adds the plugin prefix automatically. C plugins can use `NovaFixPlugin_Log`

Newlines are flattened, oversized messages are truncated and the possible results are `ACCEPTED`, `UNSUPPORTED` or `INVALID_ARGUMENT`

## Native notifications

The `novafix.native_notifications` service is available on supported builds of all three games

There are two modes:

- `TIMED` shows a normal non-modal game message and closes it after `durationMs`, confirm/cancel input won't affect it
- `ACKNOWLEDGE` shows a native message with an `OK` choice and temporarily owns gameplay input until it's confirmed

Both support `delayMs`, a 3x3 screen position and inline button glyphs. Available positions go from `TOP_LEFT` through `BOTTOM_RIGHT`, while `DEFAULT` keeps the game's normal placement which is currently centered

For compatibility with older versions, check `CAP_POSITION_GRID` before relying on custom positioning

Notifications are queued and processed asynchronously, `ACCEPTED` means it got queued and `BUSY` means the queue is full

Native notification text can use semantic button glyphs:

```cpp
plugin.notifyTimed(
    "Press " NOVAFIX_PLUGIN_GLYPH_CONFIRM " to continue or "
    NOVAFIX_PLUGIN_GLYPH_CANCEL " to go back.",
    4000u);
```

Available glyph macros:

- `GLYPH_CONFIRM`
- `GLYPH_CANCEL`
- `GLYPH_FACE_LEFT`
- `GLYPH_FACE_TOP`
- `GLYPH_MENU`

These follow whatever prompt layout the game is using, so no hard-coded Xbox/PlayStation artwork. The raw markup is `{button:confirm}` and so on, use `{{` if a literal `{` is needed

Older hosts might not have inline glyphs, check `CAP_INLINE_GLYPHS` if that matters for your plugin

C++ plugins can pass the notification position directly to `notifyTimed()` and `notifyAcknowledged()`

C plugins can use:

```text
NovaFixPlugin_GetNativeNotificationService()
NovaFixPlugin_Notify()
```

The old `showNativeMessage` fields are still supported for ABI compatibility

## Lightning Returns native menu

Lightning Returns plugins can add entries to the game's native menu

Supported item types are:

- `ACTION`
- `CHECKBOX`
- `RADIO`
- `SEPARATOR`
- `SUBMENU`

`parentIndex = -1` places an item at the root, any other `parentIndex` points to an earlier `SUBMENU`

These callbacks run on the Lightning Returns window thread. FFXIII and FFXIII-2 ignore native menu descriptors

Raw C helpers:

```text
NovaFixPlugin_MakeNativeMenuItem
NovaFixPlugin_MakeNativeMenu
NovaFixPlugin_SetNativeMenus
```

## ABI and lifetime

Plugins expose one discovery function:

```c
NOVAFIX_PLUGIN_EXPORT const NovaFixPluginDescriptor* NOVAFIX_PLUGIN_CALL
NovaFixPlugin_Query(const NovaFixPluginHost* host);
```

Return `NULL` if the plugin doesn't want to register. `NovaFixPlugin_Query` runs after `DllMain`, don't let C++ exceptions cross the query or callback boundary

`NOVAFIX_PLUGIN_ABI_VERSION` is currently `1`, `NOVAFIX_PLUGIN_API_VERSION` is kept as a source-compatible alias

ABI 1 is extended by appending fields rather than changing the existing prefixes

Structure|Frozen prefix
-|-
`NovaFixPluginControl`|56 bytes, through `options`
`NovaFixPluginDescriptor`|40 bytes, through `controlStride`
`NovaFixPluginHost`|28 bytes, through `showNativeMessageEx`
`NovaFixPluginNativeMenu`|24 bytes
`NovaFixPluginNativeMenuItem`|32 bytes

Newer API features are added through appended fields, explicit array strides, control extension chains and separately versioned services

Plugins should check whether an optional field or service is actually there before using it, don't depend on an exact structure size. Older ABI 1 plugins still work on current hosts, but obviously if a plugin depends on some newer optional feature it'll need a newer host

The host copies descriptor text, choice strings and declarative extension data during plugin discovery. The host structure and its strings remain valid until process exit, callbacks, callback `userData` and raw extension records remain owned by the plugin

Extension instances belong to a single control and must not be shared between controls. Each control needs its own extension record, though callbacks and `userData` may be shared.

For raw C extension chains, the first valid record of each type wins. `NovaFixPlugin_AddControlExtension` prepends records, so the most recently added record of a given type takes priority

The checked `NovaFixPlugin_*` helpers are there so you don't have to repeat `structSize` checks everywhere, `plugin_abi_contract.cpp` verifies the frozen ABI layouts at build time

## Executable support

See [`HOOKS_AND_PATCHES.md`](HOOKS_AND_PATCHES.md) for supported game builds and executable-specific details