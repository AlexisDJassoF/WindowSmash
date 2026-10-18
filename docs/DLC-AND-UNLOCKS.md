# DLC and fighter unlocks

[README](../README.md) · [Build guide](BUILDING.md) · [Multiplayer](../NETPLAY.md)

Installed DLC reporting, compiled fighter code, and base-game save unlocks are separate parts of setup.

## Version boundary

The current target is **11.0.0**. Its content includes Piranha Plant, Joker, Hero, Banjo & Kazooie, Terry, Byleth, Min Min, Steve, Sephiroth, and Pyra/Mythra. **Kazuya and Sora require newer game content** and cannot be added by a configuration file or save flag.

The development build extracted and compiled the 96 modules found in this target archive's `prebuilt:/nro/release/` folder. This includes shared logic, item code, and special-mode modules as well as fighters. It is not a claim that every fighter/stage or special mode has been match-tested.

## Installed-content reporting

The runtime's `nn::aoc` implementation reads installed add-on indices from `dump/modules/aoc.cfg`. Without that file, it reports no installed DLC. Generate it from your own installed-content metadata:

```powershell
python tools/nca.py aoc-cfg "D:\MyDump\registered" --keys "D:\MyKeys" --out dump/modules/aoc.cfg
```

The game data and compiled modules must match that content. For a source build, run:

```powershell
python tools/arc.py nro
python tools/recomp.py all dump/exefs --out build --jobs 16
```

A runtime-release user can reuse their own compatible existing `aoc.cfg` through **Configure Game Data.bat**. Runtime releases do not include a machine's installed-content list.

When installed content first becomes visible, the game may show fighter, stage, music, or costume notifications. Match the game version and relevant content configuration between players.

## Unlock your own base-game fighters

1. Start the configured game once and reach the main menu to create a save.
2. **Close every game instance.** The game keeps its save in memory and can overwrite a concurrent edit.
3. Double-click **`Unlock All Fighters.bat`**.
4. Read the tool's result and keep the timestamped backup in `save_backups/`.
5. Start the game again. Use the in-game **X** button to skip milestone notices; the keyboard equivalent is **C**.

The Windows helper uses built-in PowerShell and does not require Python. It refuses to run when an `ssbu` process is present, validates the known save layout, and replaces the save with a backup. Repeating it after every fighter is already unlocked makes no additional edit.

Mii Fighters may need to be created under **Games & More → Mii Fighters** before appearing on character select.

### Python and diagnostic alternatives

```powershell
python tools/unlock_all.py --dry-run
python tools/unlock_all.py
python tools/unlock_all.py --save "D:\MySave\system_data.bin" --dry-run
powershell -NoProfile -File tools/Unlock-All-Fighters.ps1 -DryRun
```

These tools target the validated **11.0.0** `system_data.bin` layout. They do not convert saves from other versions or install DLC. Keep each player's save and network identity separate.

## What the save tools change

Each base-game fighter has a selection/challenger entry and a separate joined-battle milestone. The tools set the selection flag, clear pending challenger state, assign an encounter order if needed, and fill a missing milestone. Bytes outside those tables are preserved.

To undo, close the game and copy the tool's reported backup over the corresponding `system_data.bin`. The tools never need a shared copy of somebody else's save.
