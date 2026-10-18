#!/usr/bin/env python3
"""Unlock every fighter in a save, without earning them.

Usage:
  unlock_all.py [--save <system_data.bin>] [--dry-run]

The save (default: the one the game uses, save/<user>/save_data/system_data.bin) keeps two
tables indexed by each fighter's save_no from ui_chara_db.prc (0 Mario ... 73 Mii Gunner,
74-83 the DLC fighters, which their add-on content unlocks):
  FLAGS    8 bytes per fighter: [0] 1 = selectable, [1..3] challenger state (a challenger the
           player lost to, still to be rematched), [4] the order fighters were first met.
  RECORDS  the "joined the battle" milestone, 8 bytes: bits 0-11 year, 12-15 month, 16-20 day,
           21-25 hour, 26-31 minute, 32-37 second, 48-55 how (3 for most), 63 set. The eight
           starters have none.
Each locked fighter gets what the game writes when one joins: the flag set, the challenger
state cleared, the next order number, and a milestone dated now (the game then shows the
"joined the battle!" milestones once at the next boot; X skips them). Found by comparing the
save with the roster the game shows (2026-10-17, game version 11.0.0).

A copy of the save is written first, to save_backups/ beside the save folder
(system_data.bin.before-unlock-<time>); copy it back over system_data.bin to undo.
Run it only while the game is closed: the game writes its save from memory.
"""
import argparse
import datetime
import os
import shutil
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SAVE = os.path.join(ROOT, "save", "0123456789abcdeffedcba9876543210", "save_data", "system_data.bin")
SAVE_SIZE = 0x5B4AF8
FLAGS = 0x53FF40
RECORDS = 0x54FEA8
STARTERS = {0, 1, 2, 3, 5, 6, 7, 8}   # Mario, Donkey Kong, Link, Samus, Yoshi, Kirby, Fox, Pikachu
FIGHTERS = ["Mario", "Donkey Kong", "Link", "Samus", "Dark Samus", "Yoshi", "Kirby", "Fox", "Pikachu", "Luigi",
            "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Ice Climbers", "Sheik", "Zelda",
            "Dr. Mario", "Pichu", "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom",
            "Mr. Game & Watch", "Meta Knight", "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike",
            "Pokemon Trainer", "Diddy Kong", "Lucas", "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.",
            "Toon Link", "Wolf", "Villager", "Mega Man", "Wii Fit Trainer", "Rosalina & Luma", "Little Mac",
            "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk", "Bowser Jr.", "Duck Hunt", "Ryu", "Ken", "Cloud",
            "Corrin", "Bayonetta", "Richter", "Inkling", "Ridley", "King K. Rool", "Simon", "Isabelle",
            "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner"]   # by save_no
HOW = 3


def record(t):
    return (t.year | t.month << 12 | t.day << 16 | t.hour << 21 | t.minute << 26 | t.second << 32
            | HOW << 48 | 1 << 63)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--save", default=DEFAULT_SAVE)
    ap.add_argument("--dry-run", action="store_true", help="list what would be unlocked, change nothing")
    a = ap.parse_args()

    data = bytearray(open(a.save, "rb").read())
    if len(data) != SAVE_SIZE or struct.unpack_from("<I", data, 0)[0] != SAVE_SIZE:
        raise SystemExit(f"{a.save}: not a system_data.bin of the expected layout ({len(data)} bytes)")
    flags = [data[FLAGS + 8 * n: FLAGS + 8 * n + 8] for n in range(len(FIGHTERS))]
    if any(flags[n][0] != 1 for n in STARTERS) or any(f[0] > 1 or any(f[5:]) for f in flags):
        raise SystemExit("the fighter table does not look as expected: this save's layout is not what this tool knows")
    for n, name in enumerate(FIGHTERS):
        old = struct.unpack_from("<Q", data, RECORDS + 8 * n)[0]
        if old and not old >> 63:
            raise SystemExit(f"unexpected milestone {old:#x} for {name}: this save's layout is not what this tool knows")

    rec = record(datetime.datetime.now())
    order = max(f[4] for f in flags)
    added = []
    for n, name in enumerate(FIGHTERS):
        if flags[n][0]:
            continue
        if not flags[n][4]:
            order += 1
        data[FLAGS + 8 * n: FLAGS + 8 * n + 5] = bytes([1, 0, 0, 0, flags[n][4] or order])
        if not struct.unpack_from("<Q", data, RECORDS + 8 * n)[0]:
            struct.pack_into("<Q", data, RECORDS + 8 * n, rec)
        added.append(name)
    if not added:
        print("every fighter is already unlocked")
        return
    print(f"{'would unlock' if a.dry_run else 'unlocking'} {len(added)}: {', '.join(added)}")
    if a.dry_run:
        return
    # save/<user>/save_data/system_data.bin -> save_backups/ beside save/, out of the game's folders
    backups = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(a.save))))),
                           "save_backups")
    os.makedirs(backups, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = os.path.join(backups, f"system_data.bin.before-unlock-{stamp}")
    shutil.copy2(a.save, backup)
    with open(a.save, "wb") as f:
        f.write(data)
    print(f"saved; the previous save is {backup}")


if __name__ == "__main__":
    main()
