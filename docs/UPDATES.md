# Game updates

[← README](../README.md) · [Setup guide](BUILDING.md) · [Several versions](VERSIONS.md) · [DLC and unlocks](DLC-AND-UNLOCKS.md) · [Technical guide](TECHNICAL.md)

Your main WindowSmash folder holds the game version you set it up with. WindowSmash can also apply an **update file** from your own dump over that game data and build the result in a folder of its own, so you can keep and play several versions side by side.

This has been tested with **13.0.5**, the game's last update, applied over **11.0.0**. Among other things, 13.0.5 adds **Kazuya and Sora** (Challenger Packs 10 and 11). The examples below use it; other updates go the same way.

## Contents

- [Add a version](#add-a-version)
- [Good to know](#good-to-know)
- [Doing it by hand](#doing-it-by-hand)
- [How the update is applied](#how-the-update-is-applied)

## Add a version

**What you need:**

- Steps 1 to 10 of the [setup guide](BUILDING.md) done. The update is applied over the game data you connected in step 10, which must be the game's original release or an earlier version with its update merged in. (You don't need to have built it.)
- The update file (a `.nsp`) from your own Switch, for example saved as `D:\MyDump\update.nsp`.
- Optional: your DLC `.nsp` files in `D:\MyDump\dlc`.
- About 10 GB of free space.

**Steps:**

1. Open PowerShell in the WindowSmash folder ([how](BUILDING.md#step-6-open-powershell-in-the-windowsmash-folder)).
2. Run this command. Name the folder after the update's version (`--tree v13.0.5`). With no DLC, leave out `--dlc "D:\MyDump\dlc"`, and change `--jobs 8` to the number that fits your PC ([step 14](BUILDING.md#step-14-build-the-game)):

   ```powershell
   python tools/install_version.py install --tree v13.0.5 --update "D:\MyDump\update.nsp" --dlc "D:\MyDump\dlc" --keys "D:\MyKeys" --jobs 8
   ```

3. Wait. It applies the update (a few minutes), takes out the fighters, then builds the game, which takes a long time the first time. If it stops, run the same command again: it carries on where it stopped.
4. It's done when it says `done in …` and names the new `build\ssbu.exe`.
5. Double-click **`Play SSBU.bat`**. It now asks which version to start.

The first time, the new version gets a **copy** of your main save, which the game upgrades when it starts. The main save stays as it was, and from then on each version keeps its own. The fonts and the shaders already prepared are copied over too.

## Good to know

- **Multiplayer needs the same version on every PC.** A 13.0.5 player can't play with an 11.0.0 one.
- **The unlock tool** knows the save layouts of 11.0.0 and 13.0.5, which are the same, and refuses other versions. The game unlocks Kazuya and Sora itself when you have their DLC ([DLC and unlocks](DLC-AND-UNLOCKS.md)).
- **The local bundler** (`tools/bundle_ssbu.py`) only understands a plain `romfs.cfg`, so it refuses an update's folder rather than make a broken bundle. The [Linux packager](../README-LINUX.md) handles both.
- **What has been checked on 13.0.5:** the game data (every block, see [below](#how-the-update-is-applied)), a build of `main` and all 98 fighter modules, and scripted runs through the menus. The title screen reads *Ver. 13.0.5*, the unlock sequence for the new content plays, and character select shows the full roster, Kazuya and Sora included. Matches haven't been checked thoroughly.
- **Managing several versions** (folders, choosing at start, sharing, disk space) is in [Several versions](VERSIONS.md).

## Doing it by hand

`tools/install_version.py` runs the steps below and the build. To run them one at a time instead, from the WindowSmash folder (with 13.0.5 as the example):

```powershell
python tools/update.py apply "D:\MyDump\update.nsp" --base dump/modules/romfs.cfg --keys "D:\MyKeys" --tree v13.0.5
```

```powershell
python tools/nca.py aoc-cfg "D:\MyDump\dlc" --keys "D:\MyKeys" --out v13.0.5/dump/modules/aoc.cfg
```

```powershell
python tools/arc.py nro --tree v13.0.5
```

```powershell
python tools/recomp.py all v13.0.5/dump/exefs --out v13.0.5/build --tree v13.0.5 --jobs 16
```

- `apply` takes a few minutes and ends by reading the whole result back and checking every block. `python tools/update.py verify <update.nsp> --keys <dir> --tree v13.0.5` repeats that check on its own.
- `aoc-cfg` is optional. It takes an installed-content folder or a folder of DLC `.nsp` files.
- The last command builds `main` and every fighter module of that version, which takes a long time the first time.

Then copy `dump/fonts` and `dump/shadercache` into `v13.0.5/dump`. The shader cache is keyed by shader, so shaders already prepared for the main version are reused. To keep your progress, copy `save` into `v13.0.5` too.

To start that version without a launcher:

```powershell
v13.0.5\build\ssbu.exe v13.0.5\dump\modules --status 30 > v13.0.5\build\play.log 2>&1
```

`SSBU_TREE=v13.0.5 tools/menurun.sh <out dir>` runs the scripted menu test on that version.

## How the update is applied

### What it makes

With `--tree v13.0.5`:

| Folder | Contents |
| :--- | :--- |
| `v13.0.5/dump/exefs` | The update's program: `main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`. |
| `v13.0.5/dump/modules` | The updated RomFS: `romfs.cfg`, `romfs.layers` and `romfs_patch.bin` (the update's own data, about 3.6 GB). Later also `aoc.cfg` and the module images. |
| `v13.0.5/dump/nro` | That version's fighter modules (`tools/arc.py nro --tree`). |
| `v13.0.5/build` | Its build (`tools/recomp.py --tree`). |
| `v13.0.5/save` | Its own save. |

Git ignores version folders, as it does `dump/` and `build/`.

### Patching a merged base

An update's RomFS is a patch over the RomFS of the game's **original release**: a map of the new image whose pieces are either the update's own data or bytes of the original release at given places.

If your base RomFS is the original release, unpacked, those places are right. `update.py` checks that first, and then applies the patch exactly. If your base already has an earlier update merged in (for example 11.0.0 rebuilt into one plain file), they aren't, and applying the patch the usual way would give a corrupt game. So `update.py` finds the pieces again:

1. **Files.** data.arc's file tables, in both versions, give every file's place in the new image and in your base. A file the patch takes from the original release is found under its path in the base, or as another file of exactly the same size. Music and movies come from their own table. The padding between files is taken from its neighbours, or is zeros.
2. **Checking.** The update carries a SHA-256 hash for every 16 KB block of the new image, so every guess is checked. A block that fails is solved again with more candidates: the shift of the nearest confirmed data in the original release's own order, the same bytes elsewhere in the image, or zeros. First one piece at a time, then several.

With 13.0.5 over a merged 11.0.0 base, **1,074,269 of 1,074,277 blocks matched the update's hashes**. The other 8 blocks (128 KB) are inside a few models' material, texture and helper-bone files whose original-release bytes no longer exist in a merged base. They keep the best guess and are listed in `romfs.unverified.txt`. None of them is in a fighter module. With the original release's RomFS as the base, the patch applies exactly.

Some updates don't carry every hash. For a block that is unchanged since the original release, they reuse that release's hash, which isn't in the update. 13.0.5 carries all of its own. Such blocks are checked through the level of the hash tree above: one hash covers the hashes of 512 blocks, and the tree goes up to a single hash in the update's header. Over a merged base, a group of 512 that doesn't match has its pieces tried other ways until it does. A group that still doesn't match is listed in `romfs.unverified.txt` as all 512 blocks, even if only one or two of them are wrong.

### Reading it

The runtime reads the result in place (`runtime/hle_fs.c`). `romfs.cfg` starts with the word `layered`, then names the layer map and its sources: your base file and `romfs_patch.bin`. Nothing else is extracted, and the plain `romfs.cfg` format still works.
