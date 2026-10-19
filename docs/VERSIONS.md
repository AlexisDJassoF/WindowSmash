# Several game versions

[← README](../README.md) · [Setup guide](BUILDING.md) · [Game updates](UPDATES.md) · [Technical guide](TECHNICAL.md)

One WindowSmash folder can hold several versions of the game side by side, each in a folder of its own with its own save. Each is installed and built with one command, and what versions have in common is shared instead of built twice.

## Contents

- [Choosing a version when you start](#choosing-a-version-when-you-start)
- [Installing a version](#installing-a-version)
- [The folders](#the-folders)
- [Disk space](#disk-space)
- [What versions share](#what-versions-share)
- [The RomFS: exact or rebuilt](#the-romfs-exact-or-rebuilt)
- [Code tied to one version](#code-tied-to-one-version)
- [The imports check](#the-imports-check)

## Choosing a version when you start

- **`Play SSBU.bat`** lists the versions that are built and asks which one to start. The one you picked last is the default (remembered in `last_version.txt`). With only one built, it starts that one straight away.
- **`launchers\Host SSBU Netplay.bat`** and **`launchers\Join SSBU Netplay.bat`** ask the same way. **Every player needs the same version.**
- **`launchers\Unlock All Fighters.bat`** asks too, and only changes a version whose save layout it knows ([DLC and unlocks](DLC-AND-UNLOCKS.md)).

To skip the question, set `SSBU_TREE` to a version's folder before starting a launcher.

## Installing a version

One command, from the WindowSmash folder, with your own update file, DLC and keys (example paths):

```powershell
python tools/install_version.py install --tree v13.0.5 --update "D:\MyDump\update.nsp" --dlc "D:\MyDump\dlc" --keys "D:\MyKeys"
```

The [game updates guide](UPDATES.md#add-a-version) walks through it step by step.

| Option | Effect |
| :--- | :--- |
| `--update` | The update file (a Patch NSP). Without it, the base game itself goes into the folder (for example `--tree v11.0.0`). |
| `--dlc` | A folder of DLC `.nsp` files, or an installed-content folder. Without it, no DLC. |
| `--keys` | The folder with `prod.keys`. Can be left out when `SSBU_KEYS` names it. |
| `--base` | Another base `romfs.cfg`. By default, the main folder's `dump/modules/romfs.cfg`. |
| `--jobs` | How many compile jobs the build runs at once (default 20). |
| `--redo romfs` | Do these steps again anyway (any steps, separated by commas). |
| `--no-build` | Stop before the build. |
| `--force` | Install over the main folder, or over a folder made before this tool existed. It refuses both otherwise. |

It goes through these steps in order:

1. **exefs:** the update's program, or the base's.
2. **romfs:** the update applied over the base's RomFS (`tools/update.py`, a few minutes; see [how the update is applied](UPDATES.md#how-the-update-is-applied)), or for the base itself its own `romfs.cfg`.
3. **aoc:** `aoc.cfg`, from the DLC packages in `--dlc`.
4. **nro:** every fighter module, taken out of that version's data.arc.
5. **extras:** `dump/fonts` and `dump/shadercache` copied from the main folder's `dump/`, and `save/` from its `save/`, each only if the version's folder has none yet.
6. **version:** `version.txt`.
7. **build:** `tools/recomp.py all`, which first checks every module's imports, then builds `build/ssbu.exe`, reusing what other versions already built.

A step whose inputs haven't changed since the last run is skipped (recorded in `install.json`). Running the command again carries on where it stopped, or just rebuilds.

`python tools/install_version.py list` shows the versions. `describe --tree <folder>` writes `version.txt` for a folder that has none; until then, the launchers show the folder's name.

## The folders

The main folder (`dump/`, `build/`, `save/`) holds the base game. Every other version has a folder named after it, such as `v13.0.5/`, laid out the same way:

| Folder or file | Contents |
| :--- | :--- |
| `dump/` | The game: `exefs/` (its program), `modules/` (`romfs.cfg`, the RomFS files, and `aoc.cfg` for DLC), `nro/` (the fighter modules), `fonts/`, `shadercache/`. |
| `build/` | The built game, `build/ssbu.exe`. |
| `save/` | **Its own save.** It starts as a copy of the main folder's `save/`; from then on, each version keeps its own. |
| `version.txt` | Which version it is, for example `13.0.5`, then main's build ID, data.arc's version and the update used. |
| `install.json` | What it was installed from, for the next run. |

Name new version folders `v<version>`: Git ignores those.

## Disk space

Each extra version needs:

- the update's own RomFS data (`romfs_patch.bin`, about 3.6 GB for 13.0.5);
- a copy of the shader cache (about 0.5 GB);
- its build: nothing while its files are links to another version's, about 5 GB otherwise.

Deleting a version's folder deletes only that version. Other versions keep their own links, and its addresses stay reserved in `module_bases.txt`.

## What versions share

**Modules.** The game is made of several modules: `main`, `subsdk0`, `sdk`, and about 100 fighter modules (NROs). Each has a build ID. Its built code depends only on its bytes, the address it's built for, and the tools.

`sdk` and `subsdk0` are byte-identical from 11.0.0 to 13.0.5, and fighter modules can be the same between nearby updates. When a version needs a module another version's folder has already built (same build ID, same address), its build files are taken from there: as hard links when the folders are on the same drive (instant, no extra space), or as copies otherwise. The tools always write a changed file as a new one, so one version never changes another's files.

Each module's build folder has a `module.txt` stamp: build ID, address, name, and a fingerprint of the tools that generated it (the recompiler's sources and the runtime's list of native functions). With the same fingerprint, the C isn't generated again; it's only checked for objects to compile. After a change to the recompiler, the C is generated again, but only changed files are compiled.

**Fighter module addresses.** Every module is built for a fixed address. `module_bases.txt`, in the main folder, keeps one address per build ID for all versions, seeded from each folder's `dump/nro/bases.txt`, so the same module gets the same address everywhere and can be shared.

- A new module gets an address past every other version's, so no version's modules get in another's way.
- Modules can't share a 16 MB slot, and fighter modules have 512 slots (`0x7200000000`–`0x7400000000`). Each version with all-new fighter modules takes about 100.
- When that space runs out, new modules take any slots free in their own version. That still works, but shares less.

**Fonts and shaders** are copied once per version. The shader cache is keyed by shader, so each version reuses what the others compiled.

## The RomFS: exact or rebuilt

An update's RomFS is a patch over the RomFS of the game's original release. When the base **is** that original release, unpacked as a plain NCA, the patch applies exactly. `update.py` tries that first: it checks 256 blocks across the image against the update's hashes, and if they match, takes every piece as the patch says.

When the base has an earlier update merged in, `update.py` finds each piece of the original in it instead ([how](UPDATES.md#patching-a-merged-base)). Either way, the result is checked block by block against the update's hashes at the end.

## Code tied to one version

- **Debug hooks in the runtime** (`HLE_WRAP(<address>)` in `runtime/hle_net.c`) wrap a guest function at an 11.0.0 address. Each has a code signature on the line above it: its first instructions, with addresses masked out. `tools/recomp.py wrap-sig <module> <address>` prints one.
  - In another version, the recompiler checks that the function at that address still matches.
  - If it doesn't, the recompiler looks for the signature elsewhere and wraps what it finds.
  - If it finds nothing, it leaves the hook out, and the link still works: the original's name is linked to a stub (`rt_wrap_missing`).
- **Unlock All Fighters** writes at save offsets worked out on 11.0.0. It reads which version a folder holds (main's build ID) and refuses any version whose layout it doesn't know. 13.0.5 uses the same offsets. For a new version, add its build ID to `LAYOUTS` in `tools/unlock_all.py` and `$knownLayouts` in `tools/Unlock-All-Fighters.ps1`, but only after checking that its save matches.

## The imports check

Before compiling, `recomp.py all` lists each module's imported symbols that no module exports and the runtime doesn't implement, in `build/imports.unresolved.txt`. `recomp.py check <exefs>` runs that check on its own.

- A **strong** import on that list would trap if called, so a new version that needs a new native function shows up here first.
- **Weak** ones read as 0 and are normal. 11.0.0 and 13.0.5 both have 0 strong and about 410 weak.
