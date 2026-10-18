# Run a Windows binary release

[README](../README.md) · [Releases](https://github.com/SNO2207/WindowSmash/releases) · [Multiplayer setup](../NETPLAY.md)

The Windows x64 runtime ZIP contains the prebuilt executable, required relocated module images, launchers, setup helper, unlock helper, and dependency notices. **A compiler and Python are not required to play.**

It does not include game-data archives, firmware fonts, console keys, a preconfigured installed-DLC list, a personal save, debug maps, object files, or shader caches. Supply compatible game data and fonts separately.

## Requirements

- Windows 10/11 x64 and a CPU with AVX2, FMA, and F16C.
- A Direct3D 11-capable GPU and working Windows driver.
- Your own **11.0.0 patched, unencrypted RomFS** and six shared-font `.bfttf` files.
- A writable local folder. Keep the game-data file available at its configured location.

## First-time setup

1. Download **`WindowSmash-Windows-x64-runtime.zip`** from the release, rather than GitHub's automatically generated source archives.
2. Extract the entire ZIP to a writable folder.
3. Double-click **Configure Game Data.bat**.
4. Enter the path to your patched `romfs.bin`. For a raw RomFS, keep offset **0**. If using an already unencrypted RomFS embedded in another file, enter its known byte offset.
5. Enter the folder containing your six firmware fonts. The helper validates their presence and copies them into `dump/fonts/`.
6. Optionally provide your existing compatible `aoc.cfg` for installed DLC, or leave that prompt blank.
7. Double-click **Play SSBU.bat**.

The helper validates the RomFS header, writes `dump/modules/romfs.cfg`, and preserves an existing config with a timestamped backup. It does not extract, decrypt, or apply a game update. Use the [source build preparation guide](BUILDING.md) to prepare inputs if you do not already have them.

Required fonts:

```text
nintendo_udsg-r_std_003.bfttf
nintendo_udsg-r_org_zh-cn_003.bfttf
nintendo_udsg-r_ext_zh-cn_003.bfttf
nintendo_udjxh-db_zh-tw_003.bfttf
nintendo_udsg-r_ko_003.bfttf
nintendo_ext_003.bfttf
```

## Updating an existing portable install

Close the game. Extract the runtime ZIP's contents into the existing folder containing **Play SSBU.bat**, merging folders and replacing files. The package does not contain `romfs.cfg`, fonts, saves, or a network identity, so existing local setup is retained. Run the configuration helper only if the data path or required inputs need changing.

Every player in a multiplayer session should use the same runtime release and game version. Follow the complete **[multiplayer setup guide](../NETPLAY.md)**.

## Optional fighter unlocks

After the first successful launch, close the game and use **Unlock All Fighters.bat**. It validates and backs up this PC's save. Details and version limitations are in the [DLC/unlock guide](DLC-AND-UNLOCKS.md).

## Controllers

Up to eight players can use Xbox, PlayStation, Switch Pro, and most other pads at once. Each pad that connects becomes the next free player; one unplugged and plugged back in gets its player back. The keyboard adds to player 1, with the keys listed in the [README](../README.md#play-and-connect). F2 lists the connected pads.

Pads other than Xbox controllers need `SDL3.dll` beside `build\ssbu.exe` (a build made with SDL3 puts it there; a package that includes it also has its license, `SDL3-LICENSE.txt`). Without it, XInput pads 1-4 are players 1-4.

The first start writes **`controllers.ini`** into the folder with **Play SSBU.bat**, with the defaults and an explanation of every setting:

| Section / setting | What it does |
| :--- | :--- |
| `[general]` `keyboard_player` | The player the keyboard adds to (1-8), or 0 for none. |
| `[general]` `background` | 1: pads keep working while the game's window is not focused. |
| `[players]` | Reserve a player for one pad, by its name, type, or GUID (the log names every pad as it connects). |
| `layout` | `positional` (default): buttons by position, as on a Switch pad, so the bottom face button is B. `labels`: by the printed letters. |
| `deadzone`, `rumble`, `trigger_threshold` | Stick dead zone and rumble strength in percent; how far a trigger is pulled to press ZL/ZR. |
| Remaps | `<console button> = <pad control>`, for example `ZR = rightshoulder`. |

`[device default]` applies to every pad; `[device ps5]`, `[device <name>]`, or `[device <GUID>]` applies to one kind of pad or one pad. Delete the file to get the defaults back.

## Window and display

The first start also writes **`display.ini`**. F11 or Alt+Enter switches between the window and full screen at any time.

| Setting | Values |
| :--- | :--- |
| `window` | `windowed` (default), `borderless` (covers its monitor), or `fullscreen` (borderless and on top; the monitor keeps its resolution). |
| `width`, `height` | The window's inside size at 100% display scaling (default 1280 x 720). |
| `pacing` | `clock` (default: a steady 60 frames a second), `vrr` (for G-Sync / FreeSync monitors; best with `borderless`), or `display` (follow a 60, 120, or 240 Hz monitor's refresh). |
| `latency` | How many frames the GPU may work ahead, 1-3 (default 3; lower is more responsive). |
| `hide_cursor` | 1: hide the mouse pointer after two seconds without movement. |

The picture stays 16:9, with black bars when the window's shape differs. Both files apply to every game version started from the same folder.

## Shaders

The game's shaders are compiled the first time each is used, so a new effect can appear a moment late or hitch the first time it is seen. Everything compiled is kept in `dump/shadercache/` and made ready in the background each time the game starts, so it is seen only once.

Builds from source can also compile the game's shaders ahead of time with **Prepare Shaders.bat** (Python and the build toolchain required; see the [build guide](BUILDING.md#10-optional-compile-the-shaders-ahead-of-time)). It runs once per game version and can be stopped and resumed.

## Problem reports

A crash, or a freeze in which no new picture appears for five seconds, writes a report into the **`reports\`** folder: `crash_<time>.txt` or `freeze_<time>.txt`, with a minidump (`.dmp`) beside it. A stop with an error message writes `stopped_<time>.txt`. When reporting a problem, attach the `.txt` report and the relevant part of `build\play.log`, after checking them for local paths, player names, and network addresses. The minidump contains parts of the game's memory: share it only privately, with someone who asked for it.

## Logs and first-run behavior

The log is `build/play.log`. Shader caches are generated locally; first-use compilation can cause hitches or temporarily incomplete visuals. F1 toggles the overlay, F2 shows details, and F11 switches to full screen. If startup fails, check the configured data path, matching game version, font files, CPU requirements, and GPU driver before reporting a problem.

This is an experimental runtime release. Archive verification and local startup checks do not establish that every fighter, stage, mode, or two-PC match works.
