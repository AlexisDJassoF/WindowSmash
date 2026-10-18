# Later game versions

[README](../README.md) · [Build guide](BUILDING.md) · [Several versions](VERSIONS.md) · [Multiplayer](../NETPLAY.md) · [Technical guide](TECHNICAL.md)

The port's own tree is game version **11.0.0**. `tools/update.py` applies an update package (a
Patch NSP) from your own dump to it. The result goes into a separate folder laid out like the
port's own, so each version is built and kept on its own. It has been used with **13.0.5**, the
last update, which among other things adds Kazuya and Sora (Challenger Packs 10 and 11).

## What it makes

With `--tree v13.0.5`:

| Folder | What |
| :--- | :--- |
| `v13.0.5/dump/exefs` | The update's program: `main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`. |
| `v13.0.5/dump/modules` | The updated RomFS: `romfs.cfg`, `romfs.layers` and `romfs_patch.bin` (the update's own data, about 3.8 GB). Later also `aoc.cfg` and the module images. |
| `v13.0.5/dump/nro` | That version's fighter modules (`tools/arc.py nro --tree`). |
| `v13.0.5/build` | Its build (`tools/recomp.py --tree`). |
| `v13.0.5/save` | Its own save. |

Version folders are ignored by Git, like `dump/` and `build/`.

## How an update is applied

An update's RomFS is a patch over the RomFS of the game's **original release**. It's a map of
the new image whose pieces are either the update's own data or bytes of the original release
at given places.

If your base RomFS is the original release, unpacked, those places are right: `update.py`
checks that first, and then applies the patch exactly. If it's an image that
already has an earlier update merged in, for example 11.0.0 rebuilt into one plain file, they
aren't, and applying the patch the usual way would give a corrupt game. `update.py` finds the
pieces again:

1. **Files.** data.arc's file tables, in both versions, give every file's place in the new image
   and in your base. A file the patch takes from the original release is found under its path
   in the base, or as another file of exactly the same sizes. Music and movies come from their
   own table. The padding between files is taken from its neighbours, or is zeros.
2. **Checking.** The update carries a SHA-256 hash for every 16 KB block of the new image, so
   every guess is checked. A block that fails is solved again with more candidates: the shift
   of the nearest confirmed data in the original release's own order, the same bytes elsewhere
   in the image, zeros. First one piece at a time, then several.

With 13.0.5 over a merged 11.0.0 base, **1,074,269 of 1,074,277 blocks matched the update's
hashes**. The other 8 blocks (128 KB) are inside a few models' material, texture and
helper-bone files whose original-release bytes no longer exist in a merged base. They keep the
best guess and are listed in `romfs.unverified.txt`. None of them is in a fighter module. With
the original release's RomFS as the base, the patch applies exactly.

The runtime reads the result in place (`runtime/hle_fs.c`). `romfs.cfg` starts with `layered`,
then names the layer map and its sources: your base file and `romfs_patch.bin`. Nothing else is
extracted, and the plain `romfs.cfg` format still works.

## Steps

`tools/install_version.py` does all of the steps below, and the build, in one command
([several game versions](VERSIONS.md)):

```powershell
python tools/install_version.py install --tree v13.0.5 --update "D:\MyDump\update.nsp" --dlc "D:\MyDump\dlc" --keys "D:\MyKeys"
```

Step by step instead, from the checkout, with your own update package, DLC and keys (example
paths):

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

- `apply` takes a few minutes and ends by reading the whole image back and checking every
  block. `python tools/update.py verify <update.nsp> --keys <dir> --tree v13.0.5` repeats that
  check on its own.
- `aoc-cfg` is optional. It takes a registered-content folder or a folder of DLC `.nsp` files.
- The last command builds `main` and every fighter module of that version, which takes a long
  time on the first run.

Copy `dump/fonts` and `dump/shadercache` into `v13.0.5/dump`. The shader cache is keyed by
shader, so the shaders already compiled for 11.0.0 are reused. To keep your progress, copy
`save` into `v13.0.5` too. The game upgrades that copy on its first boot, and the 11.0.0 save in
`save` stays as it was.

Start that version from the checkout:

```powershell
v13.0.5\build\ssbu.exe v13.0.5\dump\modules --status 30 > v13.0.5\build\play.log 2>&1
```

The launchers list every version that's built and ask which one to start
([several game versions](VERSIONS.md)). `SSBU_TREE=v13.0.5 tools/menurun.sh <out dir>` runs the
scripted menu test on that version.

## Good to know

- **Netplay needs the same version on every PC.** A 13.0.5 player can't join an 11.0.0 one.
- **The unlock tool** (`Unlock All Fighters.bat`) knows 11.0.0's and 13.0.5's save layouts,
  which are the same, and refuses other versions. The game unlocks Kazuya and Sora itself when
  their DLC is installed ([DLC/unlock guide](DLC-AND-UNLOCKS.md)).
- **The local bundler** (`tools/bundle_ssbu.py`) only knows the plain `romfs.cfg`. It refuses a
  layered one rather than make a broken bundle.
- **Checked on 13.0.5:** the RomFS (every block, as above), a build of `main` and all 98 fighter
  modules, and menu-only scripted runs: the title screen reads Ver. 13.0.5, the unlock sequence
  for the new content plays, and character select shows the full roster, Kazuya and Sora
  included. Matches haven't been checked comprehensively.
