# Playing

[← README](../README.md) · [Setup guide](BUILDING.md) · [Multiplayer](../NETPLAY.md) · [DLC and unlocks](DLC-AND-UNLOCKS.md)

Everything about playing once the game is built: how to start it, the controls, the settings files, and where WindowSmash keeps your save, screenshots and problem reports.

## Contents

- [Starting the game](#starting-the-game)
- [Controls](#controls)
- [Controllers](#controllers)
- [Window and display](#window-and-display)
- [Shaders and first-time stutter](#shaders-and-first-time-stutter)
- [Where your files are](#where-your-files-are)
- [Problem reports](#problem-reports)

## Starting the game

Everything starts from a launcher in the WindowSmash folder. Double-click the one you want:

| Launcher | What it does |
| :--- | :--- |
| **`Play SSBU.bat`** | Starts the game and joins the online lobby, so Local Wireless shows everyone's rooms. If the lobby doesn't answer, the game plays offline. |
| **`Play SSBU Offline.bat`** | Starts the game without going online. |
| **`Host SSBU Netplay.bat`** | Starts the game as the host of a private room ([multiplayer guide](../NETPLAY.md#private-room)). |
| **`Join SSBU Netplay.bat`** | Starts the game and joins someone's private room. |
| **`Unlock All Fighters.bat`** | Unlocks every base-game fighter on your save. Close the game first ([DLC and unlocks](DLC-AND-UNLOCKS.md#unlock-every-base-game-fighter)). |
| **`Prepare Shaders.bat`** | Prepares all of the game's shaders ahead of time ([below](#shaders-and-first-time-stutter)). |
| **`Install WindowSmash.bat`** | Sets everything up and builds the game ([setup guide](BUILDING.md#install-in-one-go)). Run it again after updating WindowSmash, or to build again. |
| **`Build WindowSmash.bat`** | Builds the game from your files, when the tools are already installed ([setup guide](BUILDING.md#shortcut-build-windowsmashbat)). |
| **`Configure Game Data.bat`** | Points WindowSmash at your game data and fonts ([setup guide, step 10](BUILDING.md#step-10-connect-your-game-data)). |

With [more than one game version](VERSIONS.md) built, the launchers ask which one to start. The last one you picked is the default.

The first time `Play SSBU.bat` runs, it asks for the name other players see online and keeps it in `online_name.txt`. Edit or delete that file to change the name.

## Controls

The keyboard plays as player 1, together with player 1's pad if there is one:

| Switch button | Keyboard |
| :--- | :--- |
| Left stick | Arrow keys |
| A / B / X / Y | <kbd>Z</kbd> / <kbd>X</kbd> / <kbd>C</kbd> / <kbd>V</kbd> |
| L / R | <kbd>A</kbd> / <kbd>S</kbd> |
| ZL / ZR | <kbd>Q</kbd> / <kbd>W</kbd> |
| + / − | <kbd>Enter</kbd> / <kbd>Backspace</kbd> |
| Right stick | <kbd>I</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd> |

| Key | What it does |
| :--- | :--- |
| <kbd>F1</kbd> | Shows or hides the overlay: frame rate and, online, the connection. |
| <kbd>F2</kbd> | Shows more details, including the connected pads. |
| <kbd>F3</kbd> | Takes a screenshot. |
| <kbd>F11</kbd> or <kbd>Alt</kbd>+<kbd>Enter</kbd> | Switches between a window and full screen. |

<details>
<summary><strong>Keys for troubleshooting</strong></summary>

<br>

| Key | What it does |
| :--- | :--- |
| <kbd>F4</kbd> | Writes one frame's drawing commands to the log. |
| <kbd>F5</kbd> | Turns depth sampling on or off. |
| <kbd>F6</kbd> | Records a 10-second profile into `build\profile_*.txt`. |

</details>

## Controllers

Up to **eight players** can use pads at once: Xbox, PlayStation, Switch Pro, and most other controllers.

- Each pad that connects becomes the **next free player**. A pad that's unplugged and plugged back in gets its player back.
- Buttons go by **position**, as on a Switch controller. On an Xbox controller, the right-hand button (**B**) is the Switch's **A**, and the bottom one (**A**) is the Switch's **B**.
- Press <kbd>F2</kbd> in the game to see which pads WindowSmash found and which player each one is.

> [!NOTE]
> PlayStation, Switch Pro and most other pads need `SDL3.dll` next to `build\ssbu.exe`. [Step 12 of the setup guide](BUILDING.md#step-12-optional-support-more-kinds-of-controllers) adds it. Without it, only Xbox (XInput) pads work, as players 1–4.

### controllers.ini

The first time the game starts, it writes **`controllers.ini`** into the WindowSmash folder. Open it in Notepad to change how pads work. Every setting is explained inside the file. Changes apply the next time the game starts, and deleting the file brings back the defaults.

| Setting | What it does | Default |
| :--- | :--- | :--- |
| `keyboard_player` | Which player the keyboard adds to: 1–8, or 0 for no keyboard. | `1` |
| `background` | `1`: pads keep working while the game window isn't in front. `0`: only while it is. | `1` |
| `layout` | `positional`: buttons by position, as on a Switch pad. `labels`: by the letter printed on the pad (PlayStation pads stay positional). | `positional` |
| `deadzone` | How much of the stick's travel around the centre is ignored, 0–50 percent. | `0` |
| `rumble` | Rumble strength, 0–100 percent. `0` turns it off. | `100` |
| `trigger_threshold` | How far a trigger must be pulled to press ZL or ZR, 1–99 percent. | `25` |
| *remaps* | `<Switch button> = <pad control>`, for example `ZR = rightshoulder`. The file lists every name. | |

The file has these sections:

- **`[general]`** holds `keyboard_player` and `background`.
- **`[players]`** reserves a player for one pad, for example `2 = DualSense Wireless Controller`. The log names every pad as it connects, with its type and GUID.
- **`[device default]`** applies to every pad.
- **`[device ps5]`**, **`[device <name>]`** or **`[device <GUID>]`** applies to one kind of pad, or one pad. Types include `xbox360`, `xboxone`, `ps4`, `ps5`, `switchpro`, `joyconpair` and `gamecube`.

For example, to make every pad use the printed letters and turn rumble down on a PlayStation 5 pad:

```ini
[device default]
layout = labels

[device ps5]
rumble = 60
```

## Window and display

<kbd>F11</kbd> or <kbd>Alt</kbd>+<kbd>Enter</kbd> switches between a window and full screen at any time. The game draws its picture at 1920 × 1080 and fits it into the window, always at 16:9, with black bars when the window has another shape. Screenshots keep the full 1920 × 1080 picture.

The first start also writes **`display.ini`**, which sets how the game starts:

| Setting | Values | Default |
| :--- | :--- | :--- |
| `window` | `windowed`: a window that keeps 16:9 as you resize it. `borderless`: covers its whole monitor. `fullscreen`: borderless and always on top; the monitor keeps its resolution. | `windowed` |
| `width`, `height` | The window's inside size at 100% display scaling. | `1280`, `720` |
| `pacing` | `clock`: a steady 60 frames a second. `vrr`: for G-Sync or FreeSync monitors, each frame shown as soon as it's ready; best with `borderless`. `display`: follows a 60, 120 or 240 Hz monitor's own refresh. | `clock` |
| `latency` | How many frames the graphics card may work ahead, 1–3. Lower is more responsive. | `3` |
| `hide_cursor` | `1` hides the mouse pointer after two seconds without movement. | `1` |

`controllers.ini` and `display.ini` apply to every game version started from the same folder.

## Shaders and first-time stutter

The game's graphics programs (*shaders*) are prepared the first time each one is used, so a new effect can show a moment late, or stutter, the first time you see it. WindowSmash keeps every prepared shader in `dump\shadercache\` and loads them in the background each time the game starts, so each stutter happens only once.

To prepare them all before playing, double-click **`Prepare Shaders.bat`**. It runs once per game version, from a few minutes to about two hours, at low priority so the PC stays usable. You can close it at any time and start it again later; what's done stays done. It needs Python and Visual Studio, as for [building](BUILDING.md).

## Where your files are

All of these are in the WindowSmash folder:

| What | Where |
| :--- | :--- |
| Your save | `save\`. Another game version keeps its own, for example `v13.0.5\save\`. |
| Screenshots (<kbd>F3</kbd>) | `screenshots\` |
| The game's log | `build\play.log` |
| Crash and freeze reports | `reports\` |
| Controller and window settings | `controllers.ini`, `display.ini` |
| Your online name | `online_name.txt` |
| Your multiplayer identity | `netplay_id.txt`. Don't copy it to another PC: two PCs with the same one look like one player. |

> [!TIP]
> **Back up your save** now and then: copy the `save` folder somewhere safe while the game is closed. To restore it, close the game and copy it back.

## Problem reports

When the game crashes, or freezes with no new picture for five seconds, WindowSmash writes a report into the **`reports\`** folder:

| File | Written when |
| :--- | :--- |
| `crash_<time>.txt` | The game crashed. |
| `freeze_<time>.txt` | No new picture for 5 seconds (again at 30 and 120 seconds). |
| `stopped_<time>.txt` | The game stopped with an error message. |
| `<…>.dmp` | Next to a crash or freeze report: a minidump of the game's memory. |

When you report a problem, attach the `.txt` report and the end of `build\play.log`. Read them first: they can contain folder names, player names and internet addresses. **Share a `.dmp` file only privately**, with someone who asked for it, because it contains part of the game's memory. [Getting help](../README.md#getting-help) lists what else to include.

If the game doesn't start at all, check, in this order: the game data path ([step 10](BUILDING.md#step-10-connect-your-game-data)), that the game version matches, the six font files, that your processor has AVX2, and your graphics driver.

WindowSmash is experimental. That it starts doesn't mean every fighter, stage, mode or online match works.
