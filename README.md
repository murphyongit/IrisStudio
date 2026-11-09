<p align="center">
  <img src="docs/images/logo.png" width="128" alt="Iris Studio">
</p>

<h1 align="center">Iris Studio</h1>

<p align="center">A movie-making mod for Call of Duty demos, starting with Black Ops.</p>

<p align="center">
  <a href="https://discord.gg/9nbYUnm53s"><img src="https://img.shields.io/badge/Discord-Join%20us-5865F2?logo=discord&logoColor=white" alt="Join the Discord"></a>
  <a href="https://github.com/yerbisonline/IrisStudio/releases"><img src="https://img.shields.io/github/v/release/yerbisonline/IrisStudio?label=Download" alt="Download"></a>
</p>

Iris Studio loads into the game and puts an editor around demo playback: a timeline you can rewind, camera paths
with keyframes, characters you place and animate inside the demo, lights, visual controls and ProRes capture straight
from the game. It's built on [IWXMVM](https://github.com/reallyluckyy/IWXMVM).

**Iris Studio** is the launcher (`IrisStudio.exe`) that sets up each game's mod and starts the game with it. Each mod
is named after its game's codename: the Black Ops mod is **T5iris**.

![The editor over a Black Ops demo on Summit, in first person with a texture pack and filmtweaks](docs/images/editor.webp)

> [!WARNING]
> **Offline only.** Never join online servers with the mod loaded. Plutonium bans players who load DLLs into the game
> while playing online, so Iris Studio only starts Plutonium in LAN mode, and the injector refuses a Plutonium that
> wasn't started that way.

> [!NOTE]
> **This project is vibecoded.** Almost all of the Black Ops port and the launcher was written by an AI (Claude, through
> Claude Code), directed by one person and tested and played by three. It's also tested with unit tests and a harness
> that drives the game, but expect rough edges, and read the code before you reuse it. Bug reports are welcome, in
> [Issues](https://github.com/yerbisonline/IrisStudio/issues) or on [Discord](https://discord.gg/9nbYUnm53s).

## Supported games

| Game | Mod | Status |
| --- | --- | --- |
| Call of Duty: Black Ops (T5) | T5iris | Multiplayer demos (`.dm_8`) on [Plutonium T5](https://plutonium.pw/) in LAN mode (recommended) or the Steam executable. Every feature below. |
| Other games of the series | – | Planned. |

## Features

**Actors work inside demos.** An actor is a character you place in a demo's playback. It walks its path, plays
animations, shoots, dies and looks around, all worked out from the timeline. So when you scrub, rewind or capture, the
actor is in the same place on the same frame as the demo's players, every time.

![Two actors and a dog on paths in a demo on Firing Range, with the Actors page open](docs/images/actors.webp)

- **Playback:** a demo browser, rewinding to any point, speed control, frame stepping, freeze time, markers, undo
  and redo. The demo's kills are found in the background and listed on the Moments page, with headshots,
  collaterals, longshots and streaks.
- **Cameras:** first person, third person, free, orbit, dolly (campaths) and a bone camera that follows a bone of a
  player or actor, such as the head or a hand. Keyframes with curves and Auto Ease, and bullet time.
- **Actors:** every character of multiplayer, the campaign and Zombies, and every animation of the game. Paths with
  walk, run and sprint stretches that blend into each other, any gun with its attachments (campaign and Zombies guns
  too), dual wield, deaths, and looking at a point.
- **Killstreaks and props:** killstreaks as actors (aircraft, the Gunship, dogs, the SAM turret, a Mortar Team) that
  fire what the game's do, props from the game's models, and effects you place.
- **Players:** change the demo's players' bodies, heads, guns, camos, face paint and death animations, for everyone
  or one player.
- **Maps:** load the campaign's levels and the Zombies maps from Change Map, with their effects and fog, and make
  scenes on them with actors.
- **Look:** sun, depth of field, filmtweaks, fog, shadow quality, map effects, HUD switches, lights you place and key,
  keys for your own ReShade's effects, and texture packs built from your game's textures.
- **Live games:** host a LAN game and add bots where you look; they take the same pages as actors.
- **Capture:** ProRes 4444 XQ / 4444 / 422 HQ / 422 / 422 LT, image sequences or camera data. Render above your
  window's size and output smaller, and capture depth, normal, world-only, players-only and gun-only passes that line up
  frame for frame.

![The Look tab on Grid: a light on the timeline, a texture pack, surface depth and ReShade's effects](docs/images/look.webp)

## Installation

You need **Call of Duty: Black Ops** on PC and **[Plutonium](https://plutonium.pw/)**: install Plutonium, log in once
in its launcher and let it download T5.

### From a release

1. Download `IrisStudio-<version>.zip` from [Releases](https://github.com/yerbisonline/IrisStudio/releases) and
   unzip it anywhere.
2. Run `IrisStudio.exe`. It finds Plutonium and your Black Ops folder. If it can't find the folder, press
   **Choose folder**.
3. Press **Set up T5iris**. A summary says what will be downloaded and built and how much space it needs. **Accept**
   does it all in one go (about three minutes and up to 800 MB the first time):
   - it downloads [OpenAssetTools](https://github.com/Laupetin/OpenAssetTools) v0.34.0 and FFmpeg for capture, and
     checks each against its SHA-256;
   - it builds the character zones and the animation pack from your own game files. Your game's files aren't changed.
4. Press **Launch T5iris**. Plutonium starts in LAN mode, and the mod loads at the main menu. Press **F1** to show or
   hide the editor, and pick a demo on the Demo tab.

<p align="center"><img src="docs/images/launcher.webp" width="400" alt="Iris Studio, ready to launch T5iris"></p>

Iris Studio keeps its files in `%LOCALAPPDATA%\IrisStudio`. The mod keeps its settings, scenes and keyframes in
Plutonium's `storage\t5\iris`, which is also where it looks for demos by default. The gear opens Settings: the game
folder, the palette, your LAN name, the update check (it only shows a link, never downloads), **Rebuild zones** after a
game update, texture packs and **Remove the mod's files**.

Windows Defender or another antivirus may flag `IrisStudio.exe` or `injector.exe`, since loading a DLL into a game is
also what cheats do.

### Building it yourself

Requirements:
- Visual Studio 2022 (or its Build Tools) with the C++ desktop workload
- [DirectX SDK (June 2010)](https://www.microsoft.com/en-us/download/details.aspx?id=6812), with `DXSDK_DIR`
  pointing to it
- Python 3, only for the scripts in `tools/`

```bash
git clone --recursive https://github.com/yerbisonline/IrisStudio.git
```

```bash
cd IrisStudio
```

```bash
MSBuild IWXMVM.sln /t:launcher /p:Configuration=Release /p:Platform=x86
```

```bash
MSBuild IWXMVM.sln /t:injector /p:Configuration=Release /p:Platform=x86
```

The launcher builds `t5` first and embeds its `T5iris.dll`, so `IrisStudio.exe` (in `launcher\bin\Win32\Release`) is
all you need to run. Close the game while you build: a loaded `T5iris.dll` can't be overwritten. The unit tests run
with `tools\run-tests.ps1`, and `tools\release\package.ps1` makes the release zip with the license and third-party
notices.

**Without the launcher** (Steam, scripts): build `T5iris.dll` and `injector.exe` as above, set up the zones with
`tools/t5-characters/setup.ps1 -Game "<Black Ops folder>"`, start the game in multiplayer (Plutonium with `-lan`, or
Steam in offline mode), wait for the main menu, then run `injector.exe`. Plutonium is recommended: the Steam
executable's DRM can end the game partway through some demos.

## Default keys

Every key can be changed in Controls, which also warns when a key is used twice. **Shift + /** lists them all in the
game, and **Ctrl + K** finds any command by name.

| Key | Action |
| --- | --- |
| **F1** | Show or hide the editor |
| **Space** | Play or pause |
| **← / →** | Skip back / forward 1 s (with **Shift**, 5 s) |
| **Ctrl + ← / →** | A frame back / forward |
| **↑ / ↓** | Play faster / slower |
| **, / .** | Previous / next kill |
| **1** – **6** | First person, third person, free, orbit, dolly, bone camera |
| **F2** | First person, or back |
| **F** | Fly the free camera (**Esc** stops) |
| **W A S D**, **Q / E** | Move, down / up |
| **Shift / Ctrl** (hold) | Move faster / slower |
| **Middle mouse** | Level the camera, FOV 90 |
| **Right mouse** (hold) | Orbit (with **Shift**, pan) |
| **F4** | Reset the orbit camera |
| **K / U / J** | Campath: add a node, move the node to the camera, play |
| **B / N** | Capture range starts / ends here |
| **M** | Add a marker |
| **[ / ]** | Key before / after (while flying: previous / next area) |
| **G** | Go to an area (campaign and Zombies maps) |
| **\\** / **Shift + \\** | Show the whole demo / the capture range |
| **Alt + ← / →** | Move the selection a frame |
| **1 / 2 / 3** | While placing a path: walk, run, sprint |
| **Enter** or **Esc**, **Backspace** | Stop placing a path, remove the last point |
| **F5 / F6 / F7** | Live game: add a bot, remove a bot, clear the bodies |
| **Ctrl + Z / Ctrl + Y** | Undo / redo |
| **Ctrl + D**, **Delete** | Duplicate, delete |
| **Ctrl + S / Ctrl + O** | Save, load a demo |
| **Ctrl + = / - / 0** | Bigger UI, smaller UI, 100% |

## Project structure

| Folder | What's in it |
| --- | --- |
| [`core`](core/) | The mod itself, shared by every game: UI (Dear ImGui), timeline, cameras, actors, lights, capture |
| [`t5`](t5/) | The Black Ops binding: hooks, actors and animation in the game, character zones |
| [`launcher`](launcher/) | `IrisStudio.exe`: setup, texture packs and starting Plutonium T5 with the mod |
| [`tests`](tests/) | Unit tests of the core logic |
| [`tools/injector`](tools/injector/) | Loads `T5iris.dll` into Black Ops |
| [`tools/t5-characters`](tools/t5-characters/), [`t5-weapons`](tools/t5-weapons/), [`t5-maps`](tools/t5-maps/), [`t5-textures`](tools/t5-textures/) | Build the catalogs and zones of characters, animations, weapons, maps and textures |
| [`tools/release`](tools/release/) | Makes the release zip |

New features are written in `core` behind the `GameInterface`, so another game only needs its own binding.

## Credits

- **[IWXMVM](https://github.com/reallyluckyy/IWXMVM)** by **reallyluckyy** and its contributors (Anomaly, Res, kruumy,
  mollyaitch, with thanks to xoxor4d and Gmzorz). Iris Studio is a fork of it, and wouldn't exist without it.
- **[Plutonium](https://plutonium.pw/)**, whose T5 client Iris Studio is built on top of.
- **[OpenAssetTools](https://github.com/Laupetin/OpenAssetTools)** by Laupetin, which builds the character zones from
  your game files.
- **[KisakCOD](https://github.com/SwagSoftware/KisakCOD)**, the reference for the engine's animation and input code.
- **[FFmpeg](https://ffmpeg.org/)** (gyan.dev's build) for capture, and **[ReShade](https://reshade.me/)** by crosire,
  whose add-on API the mod talks to.
- Libraries: Dear ImGui, spdlog, MinHook, GLM, JSON for Modern C++, tinyobjloader, magic_enum, incbin,
  IconFontCppHeaders; fonts Rubik and Font Awesome 6 Free. Their licenses are in `NOTICES.txt` of each release.

## License

[GNU GPL v3](LICENSE), like IWXMVM.

Call of Duty and Black Ops are trademarks of Activision Publishing, Inc. Iris Studio and T5iris aren't made or
endorsed by Activision or Plutonium. The character zones and texture packs are built on your computer from your own
copy of the game, and none of the game's files are shipped.
