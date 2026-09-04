> [!NOTE]
> Executable addresses are RVAs



## Table of Contents

- [Imported functions](#imported-functions)
- [DirectInput vtables](#directinput-vtables)
- [Direct3D 9 vtables](#direct3d-9-vtables)
- [Shared game hooks](#shared-game-hooks)
- [XIII](#xiii)
- [XIII-2](#xiii-2)
- [LR](#lr)
- [Add-on modules](#add-on-modules)
- [Executable profiles](#executable-profiles)

## Imported functions



### Main executable

Module|Imports
-|-
`kernel32.dll`|`GetProcAddress`, `GetCommandLineA`, `GetCommandLineW`
`user32.dll`|`ClipCursor`, `SetCursorPos`, `GetCursorPos`, `SetCapture`, `ShowCursor`, `GetAsyncKeyState`, `PeekMessageA`, `PeekMessageW`
`dinput8.dll`|`DirectInput8Create`
`xinput1_3.dll`|ordinal 2: `XInputGetState` (LR)

`GetCommandLineA/W` also checks `api-ms-win-core-processenvironment-l1-1-0.dll`
and `api-ms-win-core-processenvironment-l1-2-0.dll`




## DirectInput vtables

Interface|Slots
-|-
`IDirectInput8A`|3: `CreateDevice`
`IDirectInputDevice8A`|2: `Release`, 9: `GetDeviceState`, 10: `GetDeviceData`




## Direct3D 9 vtables

Interface|Slots
-|-
`IDirect3D9`|16: `CreateDevice`
`IDirect3D9Ex`|20: `CreateDeviceEx`
`IDirect3DDevice9`|2: `Release`, 13: `CreateAdditionalSwapChain`, 14: `GetSwapChain`, 16: `Reset`, 17: `Present`, 26: `CreateVertexBuffer`, 69: `SetSamplerState`, 75: `SetScissorRect`, 83: `DrawPrimitiveUP`, 106: `CreatePixelShader`, 107: `SetPixelShader`, 109: `SetPixelShaderConstantF`
`IDirect3DDevice9Ex`|121: `PresentEx`, 132: `ResetEx`

Swap-chain `Present` is intercepted through a proxy, not a vtable hook

See: [`factory_hooks.cpp`](../src/d3d9/factory_hooks.cpp), [`device_hooks.cpp`](../src/d3d9/device_hooks.cpp), and [`swap_chain_proxy.cpp`](../src/d3d9/swap_chain_proxy.cpp)





## Shared game hooks

Fix|XIII RVA|XIII-2 RVA|LR RVA
-|-|-|-
Facial animation|`0x7A4F60`|`0x5C08E0`|`0x596AB0`
Controller scan|`0x422690`|`0x2A7560`|native rescan at `0x3439F0`
Vibration|`0x423400`|CALL at `0x2A7215`|CALL at `0x344525`





## XIII

Fix|Modified RVAs
-|-
Frame pacing|`0xA8D65F`
Motion blending|`0xB928D0`, `0xB94040`
Titan route|CALL `0x1CD4C1`; `0x3D6ED0`, `0x3F1470`, `0x3DFB41`
Prompt mode|`0x41FBE0`; atlas functions are signature-scanned
Prompt layout|`0x6929D0`, `0x680090`
High-res UI clipping|`0x61654C`, `0x616571`, `0x616596`, `0x6165BB`, `0x572B26`, `0x668DE9`, `0x668E1E`, `0x668E56`, `0x668E91`
Bad frames during scene cuts|`0xBDE310`, `0xBAE200`, `0xBA4AC0`, `0xBA4270`, `0xBAE100`, `0x724290`, `0x720B30`, `0x71EA40`, `0xBDC2A0`, `0x71F970`
Add-on notification|`0x7DDFB0`, `0x60B7F0`

Frame pacing also writes two timing fields through `0x243E34C`
Graceful shutdown uses virtual slot 35 on the game object at `0x243E35C`
The Nautilus workaround replaces the `d3dx9_43!D3DXCreateTexture` import





## XIII-2

Fix|Modified RVAs
-|-
Frame pacing|`0x802610`; CALLs `0x8043C2`, `0x804400`, `0x804E8A`, `0x8050EA`; wait `0x8027DA`; movie open `0x6204D0`; VFX update `0x4DEAC0`
Root motion|`0x7F1800`
Chain timing|`0x78B370`
Model resource budget|`0x35550`
Resource unwrap|`0x82FA30`
Prompt mode|`0x3A19B0`, `0x2A5D00`
Prompt layout|`0x4F7160`, `0x4EA600`, `0x492880`
QTE prompts|`0x3B22B0`, `0x3B23F0`, `0x3AC750`, `0x3B5030`
High-res HUD sampling|bytes at `0x4E013B` and `0x4E0140` inside `0x4E0135`
Fixed-depth shadows|`0x8442F0`, `0x8215B0`, `0x8219E0`
Direct shadow atlas|`0x34BE60`, `0x6418E0`, `0x821330`, `0x8447F0`
Bad frames during scene cuts|`0x8C7C10`, `0x8B02F0`, `0x8ADFA0`, `0x8AD460`, `0x8B0220`, `0x551320`, `0x551F00`, `0x550180`, `0x8C6E40`
Add-on notification|`0x492840`, `0x1DB80`
Graceful shutdown|`0x8046B0`

Replaced `steam_api` imports in `sgw.dll`:

- `SteamRemoteStorage`
- `SteamAPI_Shutdown`
- `SteamAPI_RegisterCallback`
- `SteamAPI_UnregisterCallback`
- `SteamAPI_RunCallbacks`

See: [`steam_cloud_safety.cpp`](../src/game/steam/steam_cloud_safety.cpp)




## LR

Fix|Modified RVAs
-|-
Prompt mode|`0x5243A0`, `0x86AD0`
Prompt guides|`0x4B4B60`, `0x484920`, `0x3C9C0`, `0x486C0`; CALLs `0x8DD96`, `0x48BE1`, `0x48C9B`
Add-on notification|`0x4B6690`, `0x43E560`
Schemata naming|`0x4764A0`, `0x477490`, `0x470C00`
White Chocobo naming|`0x5DA4C0`, `0x5DAD20`, `0x5DB940`
Map marker naming|CALL `0x43B81E`; `0x43DA30`
Snapshot availability|CALLs `0x338D9D`, `0x3B0877`, `0x3F40B0`, `0x3F4333`, `0x3F4460`, `0x3F4508`
Snapshot Outerworld menu|CALLs `0x587EB`, `0x587C1`, `0x4F746`; `0x50480`, `0x52F10`
Snapshot message editor|CALLs `0x584C7`, `0x52918`, `0x4FBEF`, `0x4FC02`
Snapshot capture|`0x47EC0`, `0x47F10`, `0x89A50`, `0x47E30`, `0x3934B0`

60+ FPS presentation uses D3D presentation parameters
The Add-ons menu uses `SetWindowLongPtrW` on the game window





## Add-on modules

Imports replaced for local `.ini` and `.log` routing:

- `CreateFileA/W`
- `GetPrivateProfileIntA/W`
- `GetPrivateProfileStringA/W`
- `GetPrivateProfileSectionA/W`
- `GetPrivateProfileSectionNamesA/W`
- `GetPrivateProfileStructA/W`
- `WritePrivateProfileStringA/W`
- `WritePrivateProfileSectionA/W`
- `WritePrivateProfileStructA/W`

See: [`addon_storage.cpp`](../src/compat/addons/addon_storage.cpp) and [`addon_profile_storage.cpp`](../src/compat/addons/addon_profile_storage.cpp)





## Executable profiles

Executable fingerprints are defined in:

- [`ff13/title_adapter.cpp`](../src/game/titles/ff13/title_adapter.cpp)
- [`ff13_2/title_adapter.cpp`](../src/game/titles/ff13_2/title_adapter.cpp)
- [`lightning_returns/title_adapter.cpp`](../src/game/titles/lightning_returns/title_adapter.cpp)
