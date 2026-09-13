<p align="center">
  <img src="misc/logo.png" alt="NovaFix logo" width="450">
</p>






Proxy for the PC versions of Final Fantasy XIII,
Final Fantasy XIII-2, and Lightning Returns

It fixes port-specific bugs, improves high-FPS behavior, expands graphics and
input options, and restores several features missing from Lightning Returns

Check [here](docs/HOOKS_AND_PATCHES.md) for imported functions,
vtable hooks, and executable patch locations.
[And the API info is here](docs/PLUGIN_API.md).



## Features

> [!NOTE]
> All settings available in the official launcher are also available in the in-game menu.
> The list below only covers added features.


**All games**

- Automatic keyboard/controller prompt switching
- Fast controller reconnect and XInput vibration
- Correct facial animations at high frame rates

**XIII and XIII-2**

- Borderless and borderless-fullscreen modes
- Proper monitor selection
- VSync, refresh rate, triple buffering, and frame limiting
- Fixes UI clipping and alignment at resolutions above 720p
- Faster UI rendering and anisotropic filtering without affecting pixel-art/UI textures
- Fixes occasional bad frames during hard scene cuts

**XIII**

- Fixes field movement and Titan's route at high frame rates
- Prevents the Nautilus cutscene crash at uncommon output resolutions

**XIII-2**

- High-FPS fixes for simulation timing, root motion, chain updates, and model initialization
- Fixes jerky camera movement in Chocobo races at high frame rates
- Fixes the prologue battle breaking at high-FPS
- Fixes the one-pixel HUD sampling error above 720p
- Restores alpha-tested shadow detail
- Optional faster shadow rendering
- Mip LOD bias control
- Automatic QTE prompt switching
- Fixes Steam Cloud save crashes
- Reconciles newer cross-PC and externally edited saves without silently
  overwriting the other copy
- Fixes D3D9 resource-pointer crashes

**Lightning Returns**

- Removes the forced VSync cap
- Restores offline Snapshot mode with camera controls, HUD toggle, PNG output, and Compose message
- Restores Schemata renaming
- Restores Map marker renaming
- Restores White Chocobo renaming








## Installation

> [!IMPORTANT]
> **DO NOT** combine it with FF13Fix or similar setups, **IT'S NOT COMPATIBLE**.
>
>Only the latest Steam builds are supported. Microsoft Store releases **ARE NOT**
supported yet.

> [!CAUTION]
> It is **HIGHLY RECOMMENDED** to use the [Nova Chrysalia mod loader](https://github.com/LR-Research-Team/Datalog/wiki/%5BGUIDE%5D-Installing-mods-for-Nova-Chrysalia) from the [official Discord server](https://discord.gg/mvqaETHjbh) to patch the game. It applies the 4 GB patch; without it, the game is likely to **CRASH** before even reaching gameplay. Other 4 GB patches either force you to use FF13Fix or trigger Steam DRM, so **DON'T USE THEM.**
>
> Nova Chrysalia is **NOT compatible with the Microsoft Store builds**.
>
> The mod loader is not required to keep playing the games afterward. If you're not interested in modding, just launch the game through the mod loader once and that's it.

### Windows

Copy `d3d9.dll` next to the game executable:

```text
XIII:   white_data/prog/win/bin/ffxiiiimg.exe
XIII-2: alba_data/prog/win/bin/ffxiii2img.exe
LR:     LRFF13.exe
```
If you already have FF13Fix installed (it also uses `d3d9.dll`), just overwrite it with NovaFix `d3d9.dll`

### Linux

Copy `d3d9.dll` to the same location and add this Steam launch option:

```sh
WINEDLLOVERRIDES="d3d9=n,b" %command%
```


Press `F10` in game to open the settings menu. You can also open it with a controller by holding both stick clicks or `Start + Back`, depending on your controller setup. This works on Windows too, not just on Steam Deck / Steam Machines.



<br>



## DXVK and ReShade

Direct3D 9 works by default. On Linux, leave the renderer set
to `Automatic`, unless you want to use a local DXVK build ofc.

For DXVK on Windows, place the 32-bit `d3d9.dll` next to the proxy as `dxvk.dll`
and select `Local DXVK` under Performance / Advanced.

For ReShade, create a `ReShade` directory next to NovaFix and place the
32-bit D3D9 ReShade DLL there as `ReShade\d3d9.dll`, **any other method will crash the game.**

Keep `ReShade.ini` and `reshade-shaders` in that directory as well, then
leave the renderer set to `Automatic`.




<br>



## Add-ons

The `addons` folder is not limited to add-ons made specifically for NovaFix.
Regular compatible 32-bit DLL/ASI mods can go there too, for example including things like
[XIII-2 Audio Fix](https://www.nexusmods.com/finalfantasyxiii2/mods/43) and
[Savior's Patch](https://www.nexusmods.com/lightningreturnsfinalfantasy13/mods/36).

If a mod comes as a DLL or ASI, putting it in `addons` is usually the safest
option. The filename does not matter, subdirectories work, and files are loaded
alphabetically, so prefixes such as `00_` and `10_` can be used to control load
order.


<br>


## Building

Install Visual Studio 2022 with the **Desktop development with C++** workload and **C++ CMake tools for Windows**
Open the repository folder -> Build it


<br>



## Reporting Issues

If you're reporting an issue, mention which game you're playing, what other mods or wrappers you're using, your system specs, and attach `NovaFix.log`. If the bug is easy to reproduce, please do it again with `Debug` logging enabled and attach that log too. Screenshots or videos help a lot. For save/progression issues, attach the save and steps to reproduce if you can.

Back up the complete save directory before replacing any live file.

Also, especially on laptops, make sure Windows isn't running in some low-power mode and that the charger is actually plugged in properly. All three games can lose a lot of performance on battery, and XIII-2 can get hit especially hard. The games are also very sensitive to overlays, screen capture or recording software, and background processes, so if performance looks much worse than it should, try disabling those first.

If the problem only shows up while recording, I can't really speak for other software, but with OBS I'd recommend using Game Capture instead of Window Capture or Display Capture. The games seem to handle that best.




<br>


## Credits

Special thanks to [Dendonflo](https://github.com/Dendonflo), [Surihix](https://github.com/Surihix), [BtEtta](https://github.com/BtEtta), H4NS, MorningSpice, [Null&Void](https://codeberg.org/hectormiguel1), and [Cyanea](https://www.nexusmods.com/profile/cyaneaBt/mods) for testing NovaFix and reporting bugs, both in the proxy itself and in the original games that I hadn't noticed on my own.

And thanks to the original [FF13Fix](https://github.com/rebtd7/FF13Fix) author and everyone who contributed to it. Their work made the entry point into the deeper fixes for these games a lot easier.


<br>



## The Boring Stuff

NovaFix is unofficial and is not affiliated with or endorsed by Square Enix.
It does not include game files; a legal installed copy of the game(s) is required.
