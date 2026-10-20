# WindowSmash on Linux

The same recompiled game as the Windows build, as a native x86-64 Linux program. It is
**experimental**.

- The renderer is still the Direct3D 11 one, running on Vulkan through **DXVK**.
- Shaders are compiled by **vkd3d-shader** when they aren't already in the shader cache. The
  cache from the Windows build comes with this folder.
- The window, controllers and sound use **SDL2**.

The game version is the one of the game tree it was built from: the main one, or a version
folder made by `tools/update.py` (for example 13.0.5). `dump/port.txt` says which.

> [!IMPORTANT]
> This folder holds the game's code and data. It's for your own machines, not for sharing.

## Contents

- [What you need](#what-you-need)
- [Setting up](#setting-up)
- [In a VMware virtual machine](#in-a-vmware-virtual-machine)
- [Controls](#controls)
- [Settings](#settings)
- [Other launchers](#other-launchers)
- [Saves](#saves)
- [If something goes wrong](#if-something-goes-wrong)
- [Building it](#building-it)

## What you need

| | Requirement |
| :--- | :--- |
| **Linux** | 64-bit, with glibc 2.35 or newer: Ubuntu 22.04 or later, Debian 12 or later, Fedora 36 or later. |
| **Processor** | One with AVX2 (Intel Haswell / AMD Excavator or newer). Virtual machines normally pass it through. |
| **Graphics** | A Vulkan driver. Mesa's (`mesa-vulkan-drivers`) covers AMD, Intel and, without a GPU, the software renderer *lavapipe*. NVIDIA needs its proprietary driver. |
| **Memory** | About 6 GB for the game on top of the desktop, so 12 GB or more for the whole machine is comfortable. |
| **Game data** | The files the Windows build reads its RomFS from (its `romfs.cfg`): one NCA or RomFS image, or for a version folder, the base game's file and the update's `romfs_patch.bin`. |

## Setting up

Every command below runs in a terminal **inside this folder**. To open one, open the folder in
your file manager, right-click an empty area, and choose **Open in Terminal**. Paste a command
with <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>V</kbd> and press <kbd>Enter</kbd>.

1. **Copy this folder (`ssbu-linux`) to the Linux machine.** If you have `ssbu-linux.tar`, unpack
   it there instead, because the tar keeps the programs executable:

   ```bash
   tar xf ssbu-linux.tar
   ```

   Copying the folder also works: the launchers fix the permissions.

2. **Install what the game needs from the system** (SDL2, Vulkan, zenity):

   ```bash
   bash install-deps.sh
   ```

3. **Make the game data reachable.** The game reads its RomFS in place, without extracting
   anything. For a version folder, the layer map `dump/modules/romfs.layers`, which comes with
   this folder, pieces it together from two files. Either:

   - copy those files into `ssbu-linux/game/`, or
   - in a **VMware** virtual machine, share the Windows folder that holds them instead (*VM >
     Settings > Options > Shared Folders*, set to *Always enabled*). It appears under
     `/mnt/hgfs/`, where the launchers look. If `/mnt/hgfs` is empty, mount it:

     ```bash
     sudo mkdir -p /mnt/hgfs && sudo vmhgfs-fuse .host:/ /mnt/hgfs -o allow_other
     ```

   The launchers find the files by their name and size (as they were on Windows when this
   folder was made) on their first run. They search this folder, `game/`, `/mnt/hgfs`,
   `/media`, `/mnt` and your home folder, then note where the files are in
   `dump/modules/romfs.cfg`. If a file moves, they look for it again. If one can't be found,
   they say its name and where it was on Windows.

   To point at the files yourself, which also skips searching a large shared folder, set
   `SSBU_NCA=/path/to/<the game's file>` and, for a version folder,
   `SSBU_ROMFS_PATCH=/path/to/romfs_patch.bin`. The launchers remember them after that:

   ```bash
   SSBU_ROMFS_PATCH=~/romfs_patch.bin bash run.sh
   ```

4. **Play:**

   ```bash
   bash run.sh
   ```

   The log goes to `build/play.log` and to the terminal.

`run.sh` plays online, as `Play SSBU.bat` does on Windows: the game joins the online lobby named
in `online_server.txt`, and Local Wireless lists every room that players of the same game version
create, on Windows or Linux ([NETPLAY.md](NETPLAY.md)). The first time, it asks for the name other
players see and keeps it in `online_name.txt`.

- To play offline, use `bash run-offline.sh`, or write `off` in `online_server.txt`. If the lobby
  doesn't answer, the game plays offline by itself after a few seconds.
- The word `off` in `online_direct.txt` keeps all of a room's traffic going through the lobby,
  as on Windows: no direct connections, so the other players don't learn your internet address.

## In a VMware virtual machine

- Turn on **Accelerate 3D graphics** (*VM > Settings > Display*).
- VMware gives Linux guests OpenGL but, as far as Mesa's drivers go, **no Vulkan GPU driver**. So
  DXVK runs on **lavapipe**, Mesa's software Vulkan renderer, which draws on the CPU, and the log
  says `[gfx] note: this is a software renderer`. Everything works, but the frame rate is low,
  especially in matches. A real GPU with a Vulkan driver should run at full speed, as the same
  renderer does on Windows, but that hasn't been measured on Linux yet.
- **More virtual CPU cores** help the software renderer most (*VM > Settings > Processors*). With
  4, menus run at only a few frames per second. Give the VM as many as the host can spare.
- **Enough memory:** 8 GB is tight with a desktop; 12 to 16 GB is better.
- To see what holds the frame rate back, press **F2**. *Present blocked … ms/frame* is the time
  the game waits each frame for the renderer to finish drawing, and *renderer … ms/frame*
  includes that wait. When nearly all of the renderer's time is the wait, it's the software
  renderer. In `top -H`, its threads are `llvmpipe-0`, `llvmpipe-1` and so on.
- `vulkaninfo --summary` shows which Vulkan device you have.
- Saving the VM's settings while *Shared Folders* is set to *Disabled* turns sharing off. Set it
  to *Always enabled*.
- **Multiplayer from the VM:** playing online (`run.sh`) and joining someone else's private room
  work with VMware's default NAT networking, because the game only sends to the server. Hosting a
  private room (`host-netplay.sh`) is different: other PCs can't reach a relay inside the VM.
  Either switch the VM's network adapter to *Bridged*, or forward UDP port 39420 in *Edit >
  Virtual Network Editor > VMnet8 > NAT Settings*.

## Controls

The same as on Windows.

**Gamepads:** up to 8 players on any pad SDL2 knows (Xbox, PlayStation, Switch Pro and most
others). Each pad that connects becomes the next free player, and a pad unplugged and plugged
back in gets its player back. Buttons go by position, as on a Switch pad: the bottom face button
is the console's B, and so on. **F2** lists the pads. A `gamecontrollerdb.txt` in this folder adds
SDL mappings for pads SDL doesn't know.

**Keyboard** (while the window has focus), on player 1 unless `controllers.ini` says otherwise:

| Switch button | Keyboard |
| :--- | :--- |
| Left stick | Arrow keys |
| A / B / X / Y | Z / X / C / V |
| L / R | A / S |
| ZL / ZR | Q / W |
| + / − | Enter (not with Alt) / Backspace |
| Right stick | I / J / K / L |

| Key | What it does |
| :--- | :--- |
| F1 | Overlay |
| F2 | Details |
| F3 | Screenshot (into `screenshots/`) |
| F4 | Log one frame's drawing |
| F5 | Depth sampling on or off |
| F6 | 10-second profile (`build/profile_*.txt`) |
| F7 | Switch between docked (1080p) and handheld (720p) while playing; the new mode shows on screen for 2 seconds |
| F11 or Alt+Enter | Switch between a window and full screen |

In a window, a bar above the picture shows the mode: click it to switch, as F7 does. It isn't
there in full screen.

When the game asks for text (a name, a Smash Tag), a `zenity` text box opens.

## Settings

**At every start**, before the game, a **Settings** menu shows the main display settings:

| Setting | Values |
| :--- | :--- |
| Mode | **Docked (1080p)**, as the game ships, or **Handheld (720p)**: the game believes it is in the hands and draws under half the pixels. For a slower graphics card (an integrated one); the game may also simplify some effects, as on the console. |
| Resolution scale | **50%**, **75%**, **100%**, **150%** or **200%** of the mode's resolution in each direction, shown with what it draws (`75% (960 x 540)` in handheld). The game's picture and menus stay as they are; only the number of pixels changes. Below 100% is lighter on the graphics card (the menu then recommends FSR); above it is sharper on a fast one. |
| Upscaling | **Linear** or **FSR** (AMD FidelityFX Super Resolution 1): how the picture is enlarged to a larger window. FSR keeps edges sharp; it is meant for Handheld in a 1080p (or larger) full screen. |
| Sharpness | FSR's sharpening, 0.0 (the most) to 2.0. Only with FSR. |
| VSync | **On**: every frame at the monitor's next refresh, no tearing. **Off**: shown the moment it's ready (less latency; may tear in full screen). **VRR**: for FreeSync / G-Sync monitors. |
| Window | **Windowed** or **Fullscreen**. |

Arrows or a pad's D-pad / left stick move and change; **Enter** or the pad's bottom button
plays; **Esc** or the right one starts without changes; the mouse works too (a value's left or
right half steps it). Untouched for 10 seconds, the game starts with what was saved. Play writes
what changed into `display.ini`, keeping your own comments and other settings (the first time,
the file as it was is kept as `display.ini.bak`). `menu = 0` in `display.ini`, or
`SSBU_MENU=0 ./run.sh`, starts straight away. A setting an environment variable fixes
(`SSBU_MODE`, `SSBU_UPSCALE`, `SSBU_PACING`, `SSBU_WINDOW`) is shown but can't be changed.

Two files in this folder, each written with its defaults and explanations the first time the game
starts (delete one to get the defaults back). They mean the same as on Windows
([docs/PLAYING.md](docs/PLAYING.md) in the source):

**`controllers.ini`:** the keyboard's player, whether pads work while the window isn't focused,
players kept for one pad, `layout = positional` or `labels`, dead zone, rumble strength, trigger
threshold and button remaps, for every pad or for one type (`[device ps5]`), name or GUID. The
log names each pad as it connects, with its type and GUID.

**`display.ini`:**

| Setting | On Linux |
| :--- | :--- |
| `window`, `width`, `height` | `borderless` and `fullscreen` are both the desktop's full screen on the window's monitor, which keeps its resolution. The picture stays 16:9, with black bars when the window has another shape. Unlike on Windows, the window isn't held to 16:9 while you resize it. |
| `mode = docked` (default) or `handheld` | What the menu's Mode sets; F7 switches while playing (not saved). `SSBU_MODE` overrides it. |
| `upscale = linear` (default) or `fsr`, `sharpness = 0.2` | What the menu's Upscaling and Sharpness set. FSR is used only when the window is larger than the game's picture; `SSBU_UPSCALE` overrides it. |
| `menu = 1` (default) | The start-up menu; `0` starts straight away. |
| `scale = 100` (default) | What the menu's Resolution scale sets: `50`, `75`, `100`, `150` or `200`. It holds for both modes, F7 included. `SSBU_SCALE` overrides it. |
| `pacing = vsync` | The menu's VSync On: the 60-a-second clock, each frame presented at the monitor's next refresh. |
| `pacing = clock` (default) | A steady 60 frames a second. DXVK presents each frame the moment it's ready; a compositing desktop shows it at its next refresh, and full screen without one may tear. To prevent that, start the game with `DXVK_CONFIG="dxgi.tearFree = True" ./run.sh`. |
| `pacing = vrr` | The same clock, for FreeSync / G-Sync monitors with VRR turned on in the desktop or the driver. Best with `window = fullscreen`. |
| `pacing = display` | Follows the monitor's refreshes on Windows; on Linux, it's the clock. |
| `latency`, `hide_cursor` | As on Windows: 1–3 frames the GPU may work ahead, and hiding the idle mouse pointer. |

**Shaders:** after the menu, before the game. This folder carries the Windows build's compiled shaders that the game has used, and
an index of every shader in the game (`dump/aot`, with its compiler `shaderaot`). At start, the
game compares the two: if shaders are missing from the cache, a **Preparing shaders** screen
compiles them before the game starts, with a bar and the time left. The first time that is the
whole game: about 40,000 shaders, half an hour on a 14-core desktop processor (longer with fewer cores); after that only what is new (a game update, or a new
translator: the screen then shows again). **Esc** starts the game at once: what is compiled is
kept, and the next start carries on with the rest. The full cache takes about 2 GB of disk.
`SSBU_PRECOMPILE=0 ./run.sh` starts without the screen. Shaders it can't compile ahead (those
with jump tables, and compute ones) are still compiled the first time they're needed, as is
any shader without the index; DXVK also makes each shader's Vulkan pipeline the first time it
draws, so a new effect can still show a moment late once, if less often.
The Windows build's start-up shader prewarm is off here: under DXVK it made a Vulkan pipeline for
every cached shader at once and held up the first frame (`SSBU_SHADER_PREWARM=1` turns it on).

## Other launchers

| Launcher | What it does |
| :--- | :--- |
| `run-offline.sh` | The game without going online, as `Play SSBU Offline.bat` on Windows. |
| `host-netplay.sh`, `join-netplay.sh` | A private room, as `Host/Join SSBU Netplay.bat` on Windows ([NETPLAY.md](NETPLAY.md)). Every player needs the same game version: 13.0.5 can't play with 11.0.0. |
| `unlock-all-fighters.sh` | The unlock tool, on this folder's save. Run it with the game closed. Like `Unlock All Fighters.bat`, it knows the save layouts of 11.0.0 and 13.0.5 (it reads which one this folder's game is) and changes nothing for another version. |

## Saves

`save/` is a copy of the Windows save of the same game tree (its `save\` folder), taken when this
folder was made. The two don't sync: copy `save/` back to that folder on Windows to carry progress
back.

> [!WARNING]
> Unpacking a newer `ssbu-linux.tar` over this folder replaces `save/`, so copy it somewhere
> first. It also replaces `online_server.txt`; `online_name.txt` stays.

## If something goes wrong

| What you see | What to do |
| :--- | :--- |
| *Some libraries the game needs are not installed* | Run `bash install-deps.sh`. |
| A black window, or `cannot create the window` / DXVK errors in the log | Run `vulkaninfo --summary`. If it lists no device, install `mesa-vulkan-drivers`. |
| No sound | SDL uses PulseAudio or PipeWire. Check the log for `[audio]` lines. |
| A crash | It writes `reports/crash_<date>_<time>.txt`: the guest function and host frames (`*** host exception`), also at the end of the log. Send it with `build/play.log`. When the port stops the game itself (something it can't do yet), it writes `reports/stopped_*.txt`. |
| A freeze (no new picture for 5 seconds) | It writes `reports/freeze_*.txt`, again at 30 and 120 seconds: every thread's stack and who holds the renderer's locks. `SSBU_WATCHDOG=0` turns that off. Windows also writes minidumps; Linux doesn't, so a core dump, if your system keeps them, is the closest thing. |
| A low frame rate on an integrated graphics card (the GPU at 100%: `DXVK_HUD=fps,gpuload ./run.sh` shows it) | In the start-up menu, Mode **Handheld (720p)** and Upscaling **FSR**, and play in full screen (F11): the game draws under half the pixels, and FSR enlarges them to the screen. Still too slow: Resolution scale **75%** (or 50%). |
| Stutter | `SSBU_HITCH=40 ./run.sh` logs every frame over 40 ms with what the game's main, vsync and renderer threads spent it on (`[hitch]` lines in the log). |
| Suspected trouble with write tracking | `SSBU_WRITE_WATCH=protect` makes the renderer track CPU writes with page protection instead of the kernel's write tracking (`userfaultfd` + `PAGEMAP_SCAN`, Linux 6.7 and later). Use it only to rule that out. The log's first lines say which one is in use. |

## Building it

You can build the Linux program on Ubuntu itself, from your own game files, keys and firmware.
No Windows machine is involved. It can also still be cross-compiled on Windows, next to the
Windows build ([below](#cross-compiling-on-windows)). Both ways give the same program: the same
compiler flags, linked against Ubuntu 22.04's glibc, so it runs on Ubuntu 22.04 or newer, on
Debian 12 or newer, and on the distributions based on them.

### On Ubuntu

You need about **40 GB free** and **16 GB of RAM** or more. The first build takes from one to a
few hours (with 4 jobs on less than 16 GB of RAM, the longer end); later ones take minutes. In
the WindowSmash folder:

1. **Install the build tools:**

   ```bash
   sudo apt install clang lld llvm bison flex python3-venv
   ```

2. **Set up Python for WindowSmash.** Ubuntu's own Python doesn't take extra packages, so
   WindowSmash gets an environment of its own, in `.venv`:

   ```bash
   python3 -m venv .venv
   ```

3. **Fetch the third-party parts** into `linux-toolchain/`, next to the WindowSmash folder:
   Ubuntu 22.04's libraries (the sysroot), DXVK, vkd3d and SPIRV-Headers. Each download is
   checked before it is used. The libraries come from one fixed moment of the Ubuntu archive,
   checked against its signature, so every machine gets the same ones.

   ```bash
   python3 tools/linux/fetch_toolchain.py
   ```

4. **Build:**

   ```bash
   .venv/bin/python tools/build_game.py
   ```

   It asks where your game files, keys and firmware are. When yuzu (or eden, sudachi, citron,
   suyu) or Ryujinx has them, it offers their folders, Flatpak installs included. It then does
   what [docs/BUILDING.md](docs/BUILDING.md) describes for Windows, up to the program, which is
   `build-linux/ssbu`. Stopped or failed, it carries on from where it was when you run it again.
   `--jobs N` sets how many files compile at once; `--check` only checks the tools.

5. **Play on this machine:**

   ```bash
   python3 tools/linux/package_linux.py --link
   ```

   ```bash
   bash dist-linux/ssbu-linux/run.sh
   ```

   `--link` makes `dist-linux/ssbu-linux` from links to the build instead of copies: it takes a
   few megabytes, a new build is played without running it again, and the shaders the game
   compiles are kept in `dump/shadercache`. From there, everything in this guide from
   [Setting up](#setting-up) step 2 on applies.

   To take the game to **another machine of yours**, make a folder to copy instead:
   `python3 tools/linux/package_linux.py --tar` writes `dist-linux/ssbu-linux.tar`.

The build also indexes the game's shaders (`tools/shader_aot.py --index`, about a minute:
`dump/aot/programs.pack` and its keys), so the first start shows the **Preparing shaders** screen
(see [Settings](#settings), **Shaders**) - a build made on Linux has no shader cache from a Windows
build to start from.

| Commands | What they do |
| :--- | :--- |
| `fetch_toolchain.py` | Makes `linux-toolchain/` (or the folder `SSBU_LINUX_TOOLCHAIN` names). Run again, it fetches only what is missing or changed. `--list` shows what it would fetch. |
| `build_game.py` | On Linux, `--target linux` is the default. Steps 1 to 8 are the Windows build's. Step 9 is `recomp.py gen-all` (the game's C, without compiling it for Windows), `tools/linux/build_vkd3d.py` once, and `tools/linux/build_linux.py`. |
| `package_linux.py` | `--link` to play here, `--tar` for a folder to copy: they don't go together. |

The LLVM tools are found on `PATH` (`clang`, `ld.lld`, `llvm-ar`, `llvm-nm`), else as
`/usr/lib/llvm-NN/bin` or with a version suffix (`clang-20`); `SSBU_LLVM_BIN` names their folder
outright. `SSBU_BISON` and `SSBU_FLEX` name bison and flex.

### Cross-compiling on Windows

The Linux program can also be cross-compiled on Windows, next to the Windows build. It compiles the same
generated C again, so build the game tree for Windows first (`tools/recomp.py`; for a version
folder, `--tree`). You also need Visual Studio's Clang (`clang.exe` and `ld.lld.exe`, or name
their folder in `SSBU_LLVM_BIN`) and Python 3.

The third-party parts go in `linux-toolchain/`, next to the checkout (or in the folder named by
`SSBU_LINUX_TOOLCHAIN`):

| Folder | What goes there |
| :--- | :--- |
| `sysroot/` | These Ubuntu 22.04 (jammy) amd64 packages, each `.deb`'s data archive extracted into it: `libc6`, `libc6-dev`, `linux-libc-dev`, `libgcc-s1`, `libgcc-11-dev`, `libstdc++6`, `libstdc++-11-dev`, `libsdl2-2.0-0`, `libsdl2-dev`, `libopus-dev`, `libvpx-dev`. Linking against 22.04's glibc keeps the program runnable on 22.04 and anything newer. |
| `dxvk-native/` | DXVK's native Linux build, version 3.1.1 (`dxvk-native-3.1.1-steamrt-sniper.tar.gz` from DXVK's releases), so that `dxvk-native/usr/lib` holds `libdxvk_d3d11.so.0` and `libdxvk_dxgi.so.0`. |
| `dxvk-headers/include/native/` | `include/native` from the DXVK 3.1.1 source, with mingw-directx-headers in its `directx/` folder (the commit DXVK 3.1.1 uses, `9df86f2`). |
| `src/vkd3d-2.1/` | The vkd3d 2.1 source. |
| `src/SPIRV-Headers-<version>/` | SPIRV-Headers, for example `vulkan-sdk-1.4.363.0`. |
| `winflexbison/` | win_flex_bison 2.5.25 (`win_bison.exe`, `win_flex.exe` and `data/`), for vkd3d's parsers. |
| `dxvk-win/dxvk-3.1.1/` | Optional: DXVK 3.1.1 for Windows, for `tools/linux/wintest.py`. |

Then, from the checkout, run these three in order:

```bash
python tools/linux/build_vkd3d.py linux
```

```bash
python tools/linux/build_linux.py --jobs 16
```

```bash
python tools/linux/package_linux.py --tar
```

| Command | What it does |
| :--- | :--- |
| `build_vkd3d.py` | Builds vkd3d-shader, once. |
| `build_linux.py` | Builds the game tree it finds (a built `v13.0.5` folder first, else the main one) into `build-linux/ssbu`, about 1.2 GB, with `ssbu.syms` for the profiler. `--port <tree>` names another tree. Each output folder holds one tree's game, so build another tree's into its own `--out` (for example `build-linux-11.0.0`). The first build compiles every game file again, about 25 minutes with 20 jobs; later builds only redo what changed. |
| `package_linux.py` | Makes `dist-linux/ssbu-linux` from it, and `ssbu-linux.tar` with `--tar`. `--build` and `--out` package another tree's build. |

To check the Linux renderer path without a Linux machine, `python tools/linux/wintest.py --vkd3d`
builds the Windows program with this runtime drawing through DXVK and compiling its shaders with
vkd3d-shader. `tools/linux/wintest_run.sh <program> <name>` runs it through the menus (no match)
with copies of the save and shader cache, and `tools/linux/compare_shots.py` compares two such
runs' captures.
