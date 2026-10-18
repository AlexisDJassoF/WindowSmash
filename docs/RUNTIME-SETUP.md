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

## Logs and first-run behavior

The log is `build/play.log`. Shader caches are generated locally; first-use compilation can cause hitches or temporarily incomplete visuals. F1 toggles the overlay and F2 shows details. If startup fails, check the configured data path, matching game version, font files, CPU requirements, and GPU driver before reporting a problem.

This is an experimental runtime release. Archive verification and local startup checks do not establish that every fighter, stage, mode, or two-PC match works.
