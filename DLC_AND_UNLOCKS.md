# DLC and fighter unlocks

The full guide is **[docs/DLC-AND-UNLOCKS.md](https://github.com/SNO2207/WindowSmash/blob/main/docs/DLC-AND-UNLOCKS.md)**. In short:

**Unlock every base-game fighter on your save**

1. Start the game once and wait for the main menu, so it creates a save. Close it.
2. Make sure every game window is closed.
3. Run the unlock tool: double-click `Unlock All Fighters.bat` on Windows, or run `bash unlock-all-fighters.sh` on Linux.
4. Keep the backup it makes in `save_backups/`. To undo, close the game and copy that backup back over the save it names.

The tool knows the save layouts of 11.0.0 and 13.0.5 and changes nothing for any other version.

**DLC**

The game sees the DLC listed in `dump/modules/aoc.cfg`, made from your own DLC files with `tools/nca.py aoc-cfg`. The game version decides which DLC fighters exist: 11.0.0 has them up to Pyra/Mythra; Kazuya came with 12.0.0 and Sora with 13.0.0. In multiplayer, every player should have the same game version and DLC.
