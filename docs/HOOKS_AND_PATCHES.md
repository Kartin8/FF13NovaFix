# Hooks and patches

> [!NOTE]
> Main executable addresses are RVAs
> `D` = detour, `C` = 5-byte CALL rewrite, `B#` = byte patch length, `W` = data write, `R` = read/call/validation only
> IAT and runtime COM vtable hooks don't have fixed RVAs

## Imported functions

Module|Imports
-|-
`kernel32.dll`|`GetProcAddress`, `GetCommandLineA/W`, `Sleep` (XIII, XIII-2)
`api-ms-win-core-processenvironment-l1-1-0.dll`|`GetCommandLineA/W` fallback import name
`api-ms-win-core-processenvironment-l1-2-0.dll`|`GetCommandLineA/W` fallback import name
`user32.dll`|`ClipCursor`, `SetCursorPos`, `GetCursorPos`, `SetCapture`, `ShowCursor`, `GetAsyncKeyState`, `PeekMessageA`, `PeekMessageW`
`dinput8.dll`|`DirectInput8Create`
`xinput1_3.dll`|ordinal 2: `XInputGetState` (LR)
`winmm.dll`|`timeGetTime` (LR)
`d3dx9_43.dll`|`D3DXCreateTexture` (XIII)

## DirectInput vtables

Interface|Slots
-|-
`IDirectInput8A`|3: `CreateDevice`
`IDirectInputDevice8A`|2: `Release`, 9: `GetDeviceState`, 10: `GetDeviceData`

## Direct3D 9 vtables

### `IDirect3D9`
- 16: `CreateDevice`

### `IDirect3D9Ex`
- 20: `CreateDeviceEx`

### `IDirect3DDevice9`
- 2: `Release`
- 13: `CreateAdditionalSwapChain`
- 14: `GetSwapChain`
- 16: `Reset`
- 17: `Present`
- 26: `CreateVertexBuffer`
- 57: `SetRenderState`
- 59: `CreateStateBlock`
- 60: `BeginStateBlock`
- 61: `EndStateBlock`
- 65: `SetTexture`
- 67: `SetTextureStageState`
- 69: `SetSamplerState`
- 75: `SetScissorRect`
- 83: `DrawPrimitiveUP`
- 91: `CreateVertexShader`
- 92: `SetVertexShader`
- 94: `SetVertexShaderConstantF`
- 100: `SetStreamSource`
- 106: `CreatePixelShader`
- 107: `SetPixelShader`
- 109: `SetPixelShaderConstantF`

### `IDirect3DDevice9Ex`
- 121: `PresentEx`
- 132: `ResetEx`

### `IDirect3DStateBlock9`
- 5: `Apply`

Files: [`factory_hooks.cpp`](../src/d3d9/device/factory_hooks.cpp), [`device_hooks.cpp`](../src/d3d9/device/device_hooks.cpp), [`swap_chain_proxy.cpp`](../src/d3d9/presentation/swap_chain_proxy.cpp)

## Shared game hooks

**Facial animation**
- XIII: D `0x7A4F60`
- XIII-2: D `0x5C08E0`, R cookie `0x1FA90DC`
- LR: D `0x596AB0`, R cookie `0x1F482E8`

**Controller scan**
- XIII: D `0x422690`, W devices/count `0x2411564`, `0x2411574`
- XIII-2: D `0x2A7560`, W devices/count `0x212A4A0`, `0x212A4B0`
- LR: R native rescan `0x3439F0`

**Vibration**
- XIII: D `0x423400`
- XIII-2: C `0x2A7215`, R backend `0x696070`
- LR: C `0x344525`, R backend `0x344D40`

## XIII

**Frame pacing**
- Writes: B5 `0xA8D65F`
- Refs:
  - state pointer `0x243E34C`
  - pointed fields are written

**Motion blending**
- Writes: D `0xB928D0`, `0xB94040`

**Titan route**
- Writes:
  - C `0x1CD4C1`
  - D `0x3D6ED0`, `0x3F1470`, `0x3DFB41`
- Refs:
  - native attach `0x1C1B60`
  - registry/resolve `0x22E80DC`, `0x2A18B0`
  - yaw continuation/return `0x3DFB49`, `0x3DF8ED`
  - frame manager `0x2401FAC` (`+0x4C` ms)
  - move-control fields `+0x180`, `+0x214`

**Prompt mode**
- Writes:
  - D `0x41FBE0`
  - D signature match, match `+0x1E0`, match `+0x2B0`
  - W `0x2426BF4`
- Refs: `0x2426BF0`, `0x725A10`, `0xAC1F00`

**Prompt layout**
- Writes:
  - D `0x6929D0`, `0x680090`
  - W vtable[0] `0xD9FEA8`
- Refs: `0x63B920`

**High-res UI clipping**
- Writes:
  - B3 `0x61654C`, `0x616571`, `0x616596`, `0x6165BB`
  - B5 `0x572B26`
  - B4 `0x668DE9`
  - B7 `0x668E1E`, `0x668E56`, `0x668E91`

**Bad frames during scene cuts**
- Writes: D `0xBDE310`, `0xBAE200`, `0xBA4AC0`, `0xBA4270`, `0xBAE100`, `0x724290`, `0x720B30`, `0x71EA40`, `0xBDC2A0`, `0x71F970`
- Refs:
  - vtables `0x22647A0`, `0x2260D48`, `0x21B27C0`, `0x21B2878`, `0x21B25B0`, `0x2260FE0`, `0x21B89C0`, `0x21B70F0`, `0x21B5AB0`, `0x2263DF8`, `0x21B6548`
  - step `0x246DDBC`

**Add-on notification**
- Writes: D `0x7DDFB0`, `0x60B7F0`, `0x5F74C0`
- Refs: `0x4EE9C0`, `0x4EF370`, `0x4F8F00`, `0x5F4110`, `0x5F4360`, `0x5F79A0`, `0x5F7DA0`, `0x60B4A0`, `0x2426A4C`

**Graceful shutdown**
- Refs:
  - app slot `0x243E35C`
  - virtual slot 35

**Nautilus crash**
- Writes: IAT `d3dx9_43!D3DXCreateTexture`

**Language override**
- Writes:
  - IAT `kernel32!Sleep`
  - W `0x22A19B8`, `0x22A19BC`, `0x243E361`
- Refs: checkpoint return `0xA921DF`

**Any-aspect camera**
- Writes: D `0x3BC690`, `0x3BEBC0`, `0x8E3C70`

**Any-aspect GUI/minimap**
- Writes:
  - D `0x4FB590`, `0xA988D0`, `0x605290`, `0x8D4620`, `0x8C4F20`, `0x897270`
  - C `0x505883`, `0x5058A0`, `0x60546C`, `0x605677`, `0x60584C`, `0x60606F`
- Refs: native GUI `0x505980`, `0x505B80`, `0x505FB0`, `0x4F9210`

**Any-aspect resolution**
- Writes: B2 `0xAC1E7A`, `0xAC1E9D`, `0xAC1EAB`, `0xAC1F4C`, `0xAC1F58`, `0xAC2348`, `0xAC2357`, `0xAC4C6C`, `0xAC4C78`, `0xAC4D4C`, `0xAC4D58`, `0xAC528C`, `0xAC5298`

**Any-aspect composition**
- Writes: C `0xA9884F`, `0xA49FB6`, `0x4FC3D9`
- Refs:
  - native composite `0xAAE920`
  - surface copy/stretch `0xAB3ED0`, `0xAB4090`

**Any-aspect backdrops**
- Writes: C `0x5B5F96`, `0x5B647B`, `0x63FD59`, `0x6401E5`, `0x640969`, `0x6410D0`, `0x6401C3`, `0x6406D9`, `0x640DA3`, `0x64152E`
- Refs: native GUI allocate/flush `0x53CE40`, `0x4E3BE0`

**Any-aspect movies**
- Writes:
  - D `0xC08CF0`
  - C `0xC09168`
- Refs: primitive `0xAB72F0`

**Minimap alert VFX**
- Writes: C `0x602E98`
- Refs:
  - creator `0x773E70`
  - position `0x774540`
  - viewport `0x505F70`, `0x505F90`

**Any-aspect menus**
- Writes:
  - B6 `0x4EB0BB`
  - C `0x4D86E3`, `0x668EBB`, `0x668F11`
  - D `0x5B5720`, `0x55ACA0`, `0x5662F0`, `0x572AB0`, `0x649DE0`, `0x64EA40`, `0x6509E0`, `0x653AC0`, `0x544150`
- Refs:
  - D3D9 slot 75
  - native node/scissor `0x4D6990`, `0xAABFE0`
  - manager `0x2426A4C`

**Party status strip (wide menu)**
- Writes: D `0x6637D0`
- Refs:
  - strip/main-menu vtables `0xD9F010`, `0xD549A8`
  - node resource/owner `+0xC0`, `+0xC8`
  - owner status root `+0x90`

**Mission list layout/clipping**
- Writes:
  - C `0x5BDAE8`
  - D `0x5BDA80`
- Refs: product/list/row/scrollbar vtables `0xD52578`, `0xD52708`, `0xD528A8`, `0xD9DE00`

**Enemy Intel subtype list**
- Writes: D `0x565690`
- Refs: product/list vtables `0xD4C158`, `0xD4C430`

**Menu vtable validation**
- Refs:
  - Datalog/article `0xD4C4A8`, `0xD990E8`
  - Settings/list/choice `0xD4D4D0`, `0xD4DAF0`, `0xD4D000`
  - main menu/minimap `0xD549A8`, `0xD569D0`
  - field map/legend `0xD550D0`, `0xD9ED78`
  - battle headers/Gestalt `0xD42628`, `0xD446D8`, `0xD42D30`
  - column profiles `0xD4AF78`, `0xD4B618`, `0xD4BA60`, `0xD4BFD0`, `0xD4C2D8`, `0xD4E730`, `0xD4FCB0`, `0xD509E8`, `0xD515C0`, `0xD544F0`
  - panels/text/title `0xD9C470`, `0xD9FBB0`, `0xD48098`, `0xD9F010`, `0xD9C2E8`, `0xD997B0`, `0xD9F880`, `0xD99A20`
  - shops `0xD53D90`, `0xD53920`, `0xD51E00`
  - battle HUD `0xD46338`, `0xD416B8`, `0xD46A58`, `0xD43B38`, `0xD45DA0`, `0xD42FD8`, `0xD421E0`, `0xD43400`, `0xD44418`, `0xD46CD0`, `0xD41B98`, `0xD499E8`

## XIII-2

**Frame pacing**
- Writes:
  - D `0x802610`, `0x6204D0`, `0x4DEAC0`
  - C `0x8043C2`, `0x804400`, `0x804E8A`, `0x8050EA`, `0x804E7A`, `0x8050DA`
  - B2 `0x8027DA`
- Refs:
  - state `0x4D67208`
  - delta `0x802660`
  - signal `0x6417E0`
  - game `0x1FABAA8`
  - vtables `0x9546F0`, `0x1ED86E8`

**Root motion**
- Writes: D `0x7F1800`
- Refs: quantum `0x1FA8B30`

**Chain timing**
- Writes: D `0x78B370`
- Refs: constant `0x96192C`

**Chocobo race camera**
- Writes: D `0x2C1AF0`, `0x53F540`

**Scene A Bahamut camera**
- Writes: C `0x129C4E`
- Refs:
  - context `0x129C42`
  - native `acos` `0x8EB7F6`

**Model resource budget**
- Writes: D `0x35550`
- Refs:
  - storage `0x211E370`
  - caller return `0x3334A`

**Resource unwrap**
- Writes: D `0x82FA30`
- Refs: mode flag `0x1FA8644`

**Prompt mode**
- Writes:
  - D `0x3A19B0`, `0x2A5D00`
  - W `0x4D4E2B8`
- Refs: `0x39F8E0`, `0x3A1440`, `0x3A1630`, `0x554830`, `0x4D4E2B0`, `0x2129F2C`, cookie `0x1FA90DC`

**Prompt layout**
- Writes: D `0x4F7160`, `0x4EA600`, `0x492880`
- Refs: `0x4C40A0`, vtable `0xA4C030`, cookie `0x1FA90DC`

**QTE prompts**
- Writes: D `0x3B22B0`, `0x3B23F0`, `0x3AC750`, `0x3B5030`
- Refs: `0x3B39F0`, `0x3B3BB0`, `0x3B3CF0`, `0x495480`, `0x386F00`, `0x378C70`, `0x378D60`, `0x4D4DE0C`

**High-res HUD sampling**
- Writes: B1 `0x4E013B`, `0x4E0140`
- Refs: validated block `0x4E0135`

**Fixed-depth shadows**
- Writes: D `0x8442F0`, `0x8215B0`, `0x8219E0`
- Refs:
  - continuation `0x8442FA`
  - returns `0x86FEC8`, `0x8451D8`
  - CALL `0x8451D3`
  - device `0x4D6BA88`
  - cookie `0x1FA90DC`

**Direct shadow atlas**
- Writes: D `0x34BE60`, `0x6418E0`, `0x821330`, `0x8447F0`
- Refs:
  - return `0x7E9B09`
  - device/owners `0x4D6BA88`, `0x4D6B400`, `0x4D6B410`
  - globals `0x1F4E424`, `0x4D6B526`

**Bad frames during scene cuts**
- Writes: D `0x8C7C10`, `0x8B02F0`, `0x8ADFA0`, `0x8AD460`, `0x8B0220`, `0x551320`, `0x551F00`, `0x550180`, `0x8C6E40`
- Refs:
  - vtables `0x1F02760`, `0x1EFF3D0`, `0x1E6B4D8`, `0x1E6BB40`, `0x1E6F2B0`, `0x1E71230`, `0x1E6F970`, `0x1F01C70`, `0x1E70708`
  - cookie `0x1FA90DC`

**Add-on notification**
- Writes: D `0x492840`, `0x1DB80`, `0x483660`
- Refs:
  - `0x387750`, `0x387870`, `0x3A5580`, `0x483770`, `0x485A60`, `0x492BB0`, `0x377A50`, `0x4D4DE0C`
  - return `0x7993`
  - cookie `0x1FA90DC`

**Graceful shutdown**
- Writes: D `0x8046B0`
- Refs: request `0x803B00`

**Language override**
- Writes:
  - IAT `kernel32!Sleep`
  - W `0x1F46D98`, `0x1F46D9C`, `0x4D6B365`
- Refs: checkpoint return `0x807D20`

**Any-aspect camera**
- Writes: D `0x1996A0`, `0x26B970`, `0x269CD0`, `0x6E8C20`

**Any-aspect internal resolution**
- Writes: B2 `0x82F9AA`, `0x82F9CD`, `0x82F9DB`, `0x82FAEC`, `0x82FF28`, `0x82FF37`, `0x8328DC`, `0x8328E8`, `0x8329BC`, `0x8329C8`, `0x832EAC`, `0x832EB8`

**Any-aspect GUI/minimap**
- Writes:
  - D `0x39E570`, `0x809720`, `0x81AD60`, `0x490AB0`
  - C `0x490B4D`, `0x490C69`, `0x490D1D`
- Refs:
  - native aspect `0x3A4B90`
  - camera upload `0x39C340`
  - minimap return `0x490D22`

**Any-aspect title VFX**
- Writes: D `0x6CC5E0`, `0x6A1CC0`
- Refs: `0x4D4DE0C`, `0xA42740`

**Any-aspect battle HUD**
- Writes: D `0x492770`, `0x377AD0`, `0x3DD9D0`
- Refs: `0x4D4DE0C`, `0x9E1FA0`, `0x9E3F68`, `0x9E4B78`, `0x9E4E20`, `0x9E6DD0`, `0x9E7208`, `0x9E7640`, `0x9E42B0`, `0x9E32B0`, `0x9E3548`, `0x9E5AD8`, `0x9E39C8`, `0x9E3C98`, `0x9EBE70`, `0x9EB628`

**Any-aspect movies**
- Writes:
  - D `0x61F0C0`
  - C `0x8250D9`
  - B3 `0x809FDB`
- Refs: primitive `0x824A60`

**Minimap alert VFX**
- Writes: C `0x4897FE`
- Refs:
  - creator `0x59C9C0`
  - position `0x59A900`
  - viewport `0x3A4B70`, `0x3A4B80`

**Any-aspect main menu**
- Writes: D `0x4424C0`, `0x4D3E90`, `0x4387F0`
- Refs: vtables `0x9FD4D8`, `0xA47F40`

**Any-aspect menu chrome**
- Writes: D `0x49C5C0`, `0x3F0A20`
- Refs: `0xA41F00`, `0x9EC900`, `0xA48D58`

**Any-aspect Crystarium list**
- Writes: D `0x41A3A0`
- Refs: `0x9F1DE8`, `0xA49D88`, `0xA48828`, `0xA49B08`, `0x9F9EB0`, `0x9F1B58`, `0xA47B88`, `0x9F2B58`, `0x9EFA20`

### Steam / `sgw.dll`

Replaced `steam_api` imports:

- `SteamRemoteStorage`
- `SteamAPI_Shutdown`
- `SteamAPI_RegisterCallback`
- `SteamAPI_UnregisterCallback`
- `SteamAPI_RunCallbacks`

The achievement bridge uses vtable slot 1
`ISteamUserStats` is checked at slot 6 (`GetAchievement`), 7 (`SetAchievement`) and 10 (`StoreStats`) before use

Save sync works like this:

- writes go to `%LOCALAPPDATA%` first
- upload happens on a background worker
- SHA-256 keeps track of the last successfully synced contents of each save
- startup reconciliation can tell a local-only edit from a Steam / cross-PC edit without assuming either side is always newer
- if both sides really diverged, the losing copy is preserved through the existing checkpoint/conflict path before convergence
- first save or catalog access waits up to one second for startup reconciliation
- that timeout is only paid once, if it happens late imports are disabled for that run
- Steam calls stay on the worker thread

File: [`steam_cloud_safety.cpp`](../src/game/steam/steam_cloud_safety.cpp)

## LR

**Prompt mode**
- Writes:
  - D `0x5243A0`, `0x86AD0`
  - IAT `xinput1_3` ordinal 2
- Refs: `0x20D9519`, `0x4CF69B0`

**Prompt guides**
- Writes:
  - D `0x4B4B60`, `0x484920`, `0x3C9C0`, `0x486C0`
  - C `0x8DD96`, `0x48BE1`, `0x48C9B`
- Refs: `0x4849C0`, `0x3BEC60`, `0x392DB0`, `0x484440`, `0x3BAEC0`, `0x4849E0`, `0x4844E0`, `0x47F960`, vtable `0x9F912C`, `0x353E00`, `0x3BE2C0`

**Add-on notification**
- Writes: D `0x4B6690`, `0x43E560`
- Refs:
  - modal `0x395C50`
  - message `0x3A5360`, `0x3A5480`, `0x3C1500`, `0x43F0E0`, `0x441500`, `0x44D540`, `0x353E00`, `0x3BE2C0`, `0x4CF19F4`

**Schemata naming**
- Writes: D `0x4764A0`, `0x477490`, `0x470C00`
- Refs: `0x525CA0`, `0x525CB0`, `0x471980`, `0x492700`, `0x46FC70`, `0x46D930`, `0x3A6D50`, `0x4CF19F4`

**White Chocobo naming**
- Writes: D `0x5DA4C0`, `0x5DAD20`, `0x5DB940`
- Refs: `0x398CD0`, `0x398D00`, `0x18EA00`

**Map marker naming**
- Writes:
  - C `0x43B81E`
  - D `0x43DA30`
- Refs: `0x3A55E0`, `0x43B130`, `0x43C3D0`, `0x43D990`, `0x43B6F0`, `0x43EA30`, `0x376C0`, `0x373D0`, `0x437430`, `0x3A6D50`, `0x3AE780`, `0x1F509A8`, `0x4CF19F4`

**Snapshot availability**
- Writes: C `0x338D9D`, `0x3B0877`, `0x3F40B0`, `0x3F4333`, `0x3F4460`, `0x3F4508`
- Refs: `0x395C50`, `0x276B00`, `0x1F50974`, `0x1F4F130`, `0x3BE260`, `0x3C1500`, `0x44D540`, `0x353E00`, `0x3BE2C0`, `0x3BAF00`, `0x4849C0`

**Snapshot Outerworld menu**
- Writes:
  - C `0x587EB`, `0x587C1`, `0x4F746`
  - D `0x50480`, `0x52F10`
- Refs: `0x582F0`, `0x3BA0A0`

**Snapshot message editor**
- Writes: C `0x584C7`, `0x52918`, `0x4FBEF`, `0x4FC02`
- Refs: `0x527F0`, `0x352820`, `0x3528D0`, `0x352900`

**Snapshot capture**
- Writes: D `0x47EC0`, `0x47F10`, `0x89A50`, `0x47E30`, `0x3934B0`

**Language override**
- Writes:
  - IAT `winmm!timeGetTime`
  - W `0x1EB12EC`
- Refs:
  - checkpoint return `0x3249C`
  - voice state `0x1F510C8`
  - native voice/save `0x86810`, `0x867E0`, `0x86920`

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

Files: [`addon_storage.cpp`](../src/compat/addons/addon_storage.cpp), [`addon_profile_storage.cpp`](../src/compat/addons/addon_profile_storage.cpp)

## Executable profiles

Executable fingerprints are in:

- [`ff13/title_adapter.cpp`](../src/game/titles/ff13/title_adapter.cpp)
- [`ff13_2/title_adapter.cpp`](../src/game/titles/ff13_2/title_adapter.cpp)
- [`lightning_returns/title_adapter.cpp`](../src/game/titles/lightning_returns/title_adapter.cpp)
