> [!NOTE]
> Main-executable addresses are RVAs. `D`: detour, `C`: 5-byte CALL rewrite,
> `B#`: byte patch length, `W`: data write, `R`: read/call/validation only.
> IAT and runtime COM-vtable hooks have no fixed RVA



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
`kernel32.dll`|`GetProcAddress`; `GetCommandLineA/W`
`api-ms-win-core-processenvironment-l1-1-0.dll`|`GetCommandLineA/W` fallback import name
`api-ms-win-core-processenvironment-l1-2-0.dll`|`GetCommandLineA/W` fallback import name
`user32.dll`|`ClipCursor`, `SetCursorPos`, `GetCursorPos`, `SetCapture`, `ShowCursor`, `GetAsyncKeyState`, `PeekMessageA`, `PeekMessageW`
`dinput8.dll`|`DirectInput8Create`
`xinput1_3.dll`|ordinal 2: `XInputGetState` (LR)





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
`IDirect3DDevice9`|2: `Release`, 13: `CreateAdditionalSwapChain`, 14: `GetSwapChain`, 16: `Reset`, 17: `Present`, 26: `CreateVertexBuffer`, 57: `SetRenderState`, 59: `CreateStateBlock`, 60: `BeginStateBlock`, 61: `EndStateBlock`, 65: `SetTexture`, 67: `SetTextureStageState`, 69: `SetSamplerState`, 75: `SetScissorRect`, 83: `DrawPrimitiveUP`, 92: `SetVertexShader`, 94: `SetVertexShaderConstantF`, 100: `SetStreamSource`, 106: `CreatePixelShader`, 107: `SetPixelShader`, 109: `SetPixelShaderConstantF`
`IDirect3DDevice9Ex`|121: `PresentEx`, 132: `ResetEx`
`IDirect3DStateBlock9`|5: `Apply`

See: [`factory_hooks.cpp`](../src/d3d9/factory_hooks.cpp), [`device_hooks.cpp`](../src/d3d9/device_hooks.cpp), and [`swap_chain_proxy.cpp`](../src/d3d9/swap_chain_proxy.cpp)





## Shared game hooks

Fix|XIII RVA|XIII-2 RVA|LR RVA
-|-|-|-
Facial animation|D `0x7A4F60`|D `0x5C08E0`; R cookie `0x1FA90DC`|D `0x596AB0`; R cookie `0x1F482E8`
Controller scan|D `0x422690`; W devices/count `0x2411564`, `0x2411574`|D `0x2A7560`; W devices/count `0x212A4A0`, `0x212A4B0`|R native rescan `0x3439F0`
Vibration|D `0x423400`|C `0x2A7215`; R backend `0x696070`|C `0x344525`; R backend `0x344D40`





## XIII

Fix|Writes|Refs
-|-|-
Frame pacing|B5 `0xA8D65F`|state pointer `0x243E34C`; pointed fields are written
Motion blending|D `0xB928D0`, `0xB94040`|—
Titan route|C `0x1CD4C1`; D `0x3D6ED0`, `0x3F1470`, `0x3DFB41`|`0x1C1B60`, `0x22E80DC`, `0x2A18B0`, `0x3DFB49`, `0x3DF8ED`, `0x2401FAC`
Prompt mode|D `0x41FBE0`; D signature match, match `+0x1E0`, match `+0x2B0`; W `0x2426BF4`|`0x2426BF0`, `0x725A10`, `0xAC1F00`
Prompt layout|D `0x6929D0`, `0x680090`; W vtable[0] `0xD9FEA8`|`0x63B920`
High-res UI clipping|B3 `0x61654C`, `0x616571`, `0x616596`, `0x6165BB`; B5 `0x572B26`; B4 `0x668DE9`; B7 `0x668E1E`, `0x668E56`, `0x668E91`|—
Bad frames during scene cuts|D `0xBDE310`, `0xBAE200`, `0xBA4AC0`, `0xBA4270`, `0xBAE100`, `0x724290`, `0x720B30`, `0x71EA40`, `0xBDC2A0`, `0x71F970`|vtables `0x22647A0`, `0x2260D48`, `0x21B27C0`, `0x21B2878`, `0x21B25B0`, `0x2260FE0`, `0x21B89C0`, `0x21B70F0`, `0x21B5AB0`, `0x2263DF8`, `0x21B6548`; step `0x246DDBC`
Add-on notification|D `0x7DDFB0`, `0x60B7F0`|`0x4F8F00`, `0x5F4110`, `0x5F4360`, `0x5F7DA0`, `0x60B4A0`, `0x2426A4C`
Graceful shutdown|—|app slot `0x243E35C`; virtual slot 35
Nautilus crash|IAT `d3dx9_43!D3DXCreateTexture`|—





## XIII-2

Fix|Writes|Refs
-|-|-
Frame pacing|D `0x802610`, `0x6204D0`, `0x4DEAC0`; C `0x8043C2`, `0x804400`, `0x804E8A`, `0x8050EA`, `0x804E7A`, `0x8050DA`; B2 `0x8027DA`|state `0x4D67208`; delta `0x802660`; signal `0x6417E0`; game `0x1FABAA8`; vtables `0x9546F0`, `0x1ED86E8`
Root motion|D `0x7F1800`|quantum `0x1FA8B30`
Chain timing|D `0x78B370`|constant `0x96192C`
Chocobo race camera|D `0x2C1AF0`, `0x53F540`|—
Scene A Bahamut camera|C `0x129C4E`|context `0x129C42`; native `acos` `0x8EB7F6`
Model resource budget|D `0x35550`|storage `0x211E370`; caller return `0x3334A`
Resource unwrap|D `0x82FA30`|mode flag `0x1FA8644`
Prompt mode|D `0x3A19B0`, `0x2A5D00`; W `0x4D4E2B8`|`0x39F8E0`, `0x3A1440`, `0x3A1630`, `0x554830`, `0x4D4E2B0`, `0x2129F2C`, cookie `0x1FA90DC`
Prompt layout|D `0x4F7160`, `0x4EA600`, `0x492880`|`0x4C40A0`, vtable `0xA4C030`, cookie `0x1FA90DC`
QTE prompts|D `0x3B22B0`, `0x3B23F0`, `0x3AC750`, `0x3B5030`|`0x3B39F0`, `0x3B3BB0`, `0x3B3CF0`, `0x495480`, `0x386F00`, `0x378C70`, `0x378D60`, `0x4D4DE0C`
High-res HUD sampling|B1 `0x4E013B`, `0x4E0140`|validated block `0x4E0135`
Fixed-depth shadows|D `0x8442F0`, `0x8215B0`, `0x8219E0`|continuation `0x8442FA`; returns `0x86FEC8`, `0x8451D8`; CALL `0x8451D3`; device `0x4D6BA88`; cookie `0x1FA90DC`
Direct shadow atlas|D `0x34BE60`, `0x6418E0`, `0x821330`, `0x8447F0`|return `0x7E9B09`; device/owners `0x4D6BA88`, `0x4D6B400`, `0x4D6B410`; globals `0x1F4E424`, `0x4D6B526`
Bad frames during scene cuts|D `0x8C7C10`, `0x8B02F0`, `0x8ADFA0`, `0x8AD460`, `0x8B0220`, `0x551320`, `0x551F00`, `0x550180`, `0x8C6E40`|vtables `0x1F02760`, `0x1EFF3D0`, `0x1E6B4D8`, `0x1E6BB40`, `0x1E6F2B0`, `0x1E71230`, `0x1E6F970`, `0x1F01C70`, `0x1E70708`; cookie `0x1FA90DC`
Add-on notification|D `0x492840`, `0x1DB80`|`0x3A5580`, `0x377A50`; return `0x7993`; cookie `0x1FA90DC`
Graceful shutdown|D `0x8046B0`|request `0x803B00`

Replaced `steam_api` imports in `sgw.dll`:

- `SteamRemoteStorage`
- `SteamAPI_Shutdown`
- `SteamAPI_RegisterCallback`
- `SteamAPI_UnregisterCallback`
- `SteamAPI_RunCallbacks`

The `sgw.dll` achievement bridge uses vtable slot 1. The resolved
`ISteamUserStats` object is validated at slots 6 (`GetAchievement`),
7 (`SetAchievement`), and 10 (`StoreStats`) before use.

Save writes commit to `%LOCALAPPDATA%` first and upload from a background
worker. A SHA-256 identity records the last successfully synchronized contents
of each save. Startup reconciliation can therefore distinguish a one-sided
local edit from a one-sided Steam or cross-PC edit without treating either
storage location as universally newer. Genuine divergence preserves the losing
copy through the existing checkpoint/conflict paths before convergence. The
first save or catalog access waits at most one second for healthy startup
reconciliation. A timeout is paid only once and disables late imports for that
run; Steam calls remain on the worker thread.

See: [`steam_cloud_safety.cpp`](../src/game/steam/steam_cloud_safety.cpp)




## LR

Fix|Writes|Refs
-|-|-
Prompt mode|D `0x5243A0`, `0x86AD0`; IAT `xinput1_3` ordinal 2|`0x20D9519`, `0x4CF69B0`
Prompt guides|D `0x4B4B60`, `0x484920`, `0x3C9C0`, `0x486C0`; C `0x8DD96`, `0x48BE1`, `0x48C9B`|`0x4849C0`, `0x3BEC60`, `0x392DB0`, `0x484440`, `0x3BAEC0`, `0x4849E0`, `0x4844E0`, `0x47F960`, vtable `0x9F912C`, `0x353E00`, `0x3BE2C0`
Add-on notification|D `0x4B6690`, `0x43E560`|modal `0x395C50`; message `0x3C1500`, `0x44D540`, `0x353E00`, `0x3BE2C0`
Schemata naming|D `0x4764A0`, `0x477490`, `0x470C00`|`0x525CA0`, `0x525CB0`, `0x471980`, `0x492700`, `0x46FC70`, `0x46D930`, `0x3A6D50`, `0x4CF19F4`
White Chocobo naming|D `0x5DA4C0`, `0x5DAD20`, `0x5DB940`|`0x398CD0`, `0x398D00`, `0x18EA00`
Map marker naming|C `0x43B81E`; D `0x43DA30`|`0x3A55E0`, `0x43B130`, `0x43C3D0`, `0x43D990`, `0x43B6F0`, `0x43EA30`, `0x376C0`, `0x373D0`, `0x437430`, `0x3A6D50`, `0x3AE780`, `0x1F509A8`, `0x4CF19F4`
Snapshot availability|C `0x338D9D`, `0x3B0877`, `0x3F40B0`, `0x3F4333`, `0x3F4460`, `0x3F4508`|`0x395C50`, `0x276B00`, `0x1F50974`, `0x1F4F130`, `0x3BE260`, `0x3C1500`, `0x44D540`, `0x353E00`, `0x3BE2C0`, `0x3BAF00`, `0x4849C0`
Snapshot Outerworld menu|C `0x587EB`, `0x587C1`, `0x4F746`; D `0x50480`, `0x52F10`|`0x582F0`, `0x3BA0A0`
Snapshot message editor|C `0x584C7`, `0x52918`, `0x4FBEF`, `0x4FC02`|`0x527F0`, `0x352820`, `0x3528D0`, `0x352900`
Snapshot capture|D `0x47EC0`, `0x47F10`, `0x89A50`, `0x47E30`, `0x3934B0`|—

60+ FPS presentation uses D3D presentation parameters
The Add-ons menu uses `SetWindowLongPtrW` on the game window





## Add-on modules

Imports replaced for local `.ini` routing:

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
