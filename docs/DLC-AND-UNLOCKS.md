# DLC and fighter unlocks

[README](../README.md) · [Build guide](BUILDING.md) · [Multiplayer](../NETPLAY.md)

Installed DLC reporting, compiled fighter code, and base-game save unlocks are separate parts of setup.

## Version boundary

The current target is **11.0.0**. Its content includes Piranha Plant, Joker, Hero, Banjo & Kazooie, Terry, Byleth, Min Min, Steve, Sephiroth, and Pyra/Mythra. **Kazuya and Sora require newer game content** and cannot be added by a configuration file or save flag. A later version built from your own update package, such as 13.0.5, has them ([later game versions](UPDATES.md), [several versions](VERSIONS.md)).

The development build extracted and compiled the 96 modules found in this target archive's `prebuilt:/nro/release/` folder. This includes shared logic, item code, and special-mode modules as well as fighters. It is not a claim that every fighter/stage or special mode has been match-tested.

## Installed-content reporting

The runtime's `nn::aoc` implementation reads installed add-on indices from `dump/modules/aoc.cfg`. Without that file, it reports no installed DLC. Generate it from your own installed-content metadata:

```powershell
python tools/nca.py aoc-cfg "D:\MyDump\registered" --keys "D:\MyKeys" --out dump/modules/aoc.cfg
```

The folder can also hold DLC packages as `.nsp` files. For a version folder, write its own `aoc.cfg` (`--out v13.0.5/dump/modules/aoc.cfg`); `tools/install_version.py --dlc` does that for you.

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
3. Double-click **`Unlock All Fighters.bat`**. With several game versions built, it asks which one ([several versions](VERSIONS.md)).
4. Read the tool's result and keep the timestamped backup in `save_backups/`.
5. Start the game again. Use the in-game **X** button to skip milestone notices; the keyboard equivalent is **C**.

The Windows helper uses built-in PowerShell and does not require Python. It refuses to run when an `ssbu` process is present, validates the known save layout, and replaces the save with a backup. Repeating it after every fighter is already unlocked makes no additional edit.

Mii Fighters may need to be created under **Games & More → Mii Fighters** before appearing on character select.

### Python and diagnostic alternatives

```powershell
python tools/unlock_all.py --dry-run
python tools/unlock_all.py
python tools/unlock_all.py --tree v13.0.5 --dry-run
python tools/unlock_all.py --save "D:\MySave\system_data.bin" --dry-run
powershell -NoProfile -File tools/Unlock-All-Fighters.ps1 -DryRun
```

`--tree` (`-Tree` for the PowerShell helper) works on a version folder and its own save; by default the tools use the port's own tree. They read which version a folder is from its program's build ID.

These tools target the validated `system_data.bin` layout of **11.0.0** and **13.0.5**, which use the same offsets, and refuse other versions. They do not convert saves from other versions or install DLC. Keep each player's save and network identity separate.

## What the save tools change

Each base-game fighter has a selection/challenger entry and a separate joined-battle milestone. The tools set the selection flag, clear pending challenger state, assign an encounter order if needed, and fill a missing milestone. Bytes outside those tables are preserved.

To undo, close the game and copy the tool's reported backup over the corresponding `system_data.bin`. The tools never need a shared copy of somebody else's save.

### 13.0.5's save

13.0.5 keeps 11.0.0's layout:

- The save is the same size, with the same format number at offset 4 (`0x1b`). Offset 8 holds the version that last wrote the save: `0xb000` for 11.0.0, `0xd005` for 13.0.5. When 13.0.5 loads an 11.0.0 save, it changes that field and a few counters, and moves no tables.
- Fighter entries are indexed by `save_no` from `ui/param/database/ui_chara_db.prc`: 0–73 for the base game and 74–83 for the earlier DLC fighters, the same in both versions. 13.0.5 adds 84 (Kazuya) and 85 (Sora), with their milestones after the other DLC fighters'.
- The game sets the flags of DLC fighters itself when their DLC is installed, so the tools leave entries 74 and up alone.

This was checked on save copies: a partly locked save showed exactly the fighters its flags say on 13.0.5, and after the tool ran, character select showed the full roster.
