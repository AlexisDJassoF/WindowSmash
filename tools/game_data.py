#!/usr/bin/env python3
"""Connect the game's data: what tools/Configure-Game-Data.ps1 does, for build_game.py on Windows
and on Linux alike.

Usage: game_data.py --romfs <romfs.bin> --fonts <folder> [--offset N] [--aoc <aoc.cfg>] [--dry-run]

It checks an unencrypted RomFS image (its 80-byte header at the offset, and that the tables it
names are inside the file) and the six firmware fonts, copies the fonts into dump/fonts and an
aoc.cfg, if given, into dump/modules, and writes dump/modules/romfs.cfg: the image's path, then
the offset. Whatever it replaces is kept as <name>.before-setup-<stamp>, and romfs.cfg is
replaced in one step, so a failure leaves the old one. On Windows romfs.cfg is in the system's
code page with CRLF, as the Windows runtime reads it; on Linux, UTF-8 with LF, as the launchers
write it."""
import argparse
import os
import shutil
import struct
import sys
import uuid
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_FILES = ("nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_org_zh-cn_003.bfttf",
              "nintendo_udsg-r_ext_zh-cn_003.bfttf", "nintendo_udjxh-db_zh-tw_003.bfttf",
              "nintendo_udsg-r_ko_003.bfttf", "nintendo_ext_003.bfttf")
HEADER = 80   # a RomFS header's size, and the value of its first field


class Error(Exception):
    """The data can't be used; the message says why."""


def cfg_format(windows=None):
    """(encoding, line end) of romfs.cfg on this system (or the one named)."""
    windows = os.name == "nt" if windows is None else windows
    return ("mbcs", "\r\n") if windows else ("utf-8", "\n")


def check_romfs(path, offset):
    if not os.path.isfile(path):
        raise Error("RomFS input file not found.")
    size = os.path.getsize(path)
    if offset < 0 or offset > size - HEADER:
        raise Error("RomFS byte offset is outside the file.")
    with open(path, "rb") as f:
        f.seek(offset)
        header = f.read(HEADER)
    fields = struct.unpack("<10Q", header) if len(header) == HEADER else ()
    if not fields or fields[0] != HEADER:
        raise Error("No unencrypted RomFS header at this offset. Prepare the patched image before running setup.")
    available = size - offset
    for field in (1, 3, 5, 7):   # the four tables: (start, size) each
        start, length = fields[field], fields[field + 1]
        if start > available or length > available - start:
            raise Error("RomFS metadata is outside the file.")
    if fields[9] > available:
        raise Error("Invalid RomFS data offset.")


def check_fonts(folder):
    if not os.path.isdir(folder):
        raise Error("Font directory not found.")
    for name in FONT_FILES:
        font = os.path.join(folder, name)
        if not os.path.isfile(font):
            raise Error("Missing required font: " + name)
        if os.path.getsize(font) < 8:
            raise Error("Truncated font file: " + name)


def keep_before(path, stamp):
    if os.path.exists(path):
        shutil.copyfile(path, f"{path}.before-setup-{stamp}")


def configure(romfs, fonts, offset=0, aoc=None, root=ROOT, dry_run=False, windows=None):
    """Check the data and connect it (see the module's docstring); raises Error."""
    romfs, fonts = os.path.abspath(romfs.strip().strip('"')), os.path.abspath(fonts.strip().strip('"'))
    encoding, newline = cfg_format(windows)
    try:
        romfs.encode(encoding)
    except UnicodeEncodeError:
        raise Error("The runtime cannot represent this RomFS path. Use a path supported by the Windows system code page.")
    check_romfs(romfs, offset)
    check_fonts(fonts)
    if aoc:
        aoc = os.path.abspath(aoc.strip().strip('"'))
        if not os.path.isfile(aoc):
            raise Error("Optional aoc.cfg file not found.")
    if dry_run:
        return None
    modules, font_dir = os.path.join(root, "dump", "modules"), os.path.join(root, "dump", "fonts")
    os.makedirs(modules, exist_ok=True)
    os.makedirs(font_dir, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3] + "-" + uuid.uuid4().hex
    for name in FONT_FILES:
        source, target = os.path.join(fonts, name), os.path.join(font_dir, name)
        if os.path.normcase(source) == os.path.normcase(os.path.abspath(target)):
            continue
        keep_before(target, stamp)
        shutil.copyfile(source, target)
    if aoc:
        target = os.path.join(modules, "aoc.cfg")
        if os.path.normcase(aoc) != os.path.normcase(os.path.abspath(target)):
            keep_before(target, stamp)
            shutil.copyfile(aoc, target)
    cfg = os.path.join(modules, "romfs.cfg")
    temporary = f"{cfg}.{stamp}.tmp"
    with open(temporary, "w", encoding=encoding, newline="") as f:
        f.write(f"{romfs}{newline}{offset}{newline}")
    keep_before(cfg, stamp)
    os.replace(temporary, cfg)
    return cfg


def main():
    ap = argparse.ArgumentParser(description="Check the game's data and connect it (dump/modules/romfs.cfg).")
    ap.add_argument("--romfs", required=True, help="the unencrypted romfs.bin (or a file with a RomFS inside)")
    ap.add_argument("--offset", type=int, default=0, help="where the RomFS starts in the file (0 for a romfs.bin)")
    ap.add_argument("--fonts", required=True, help="the folder with the six firmware .bfttf fonts")
    ap.add_argument("--aoc", help="an aoc.cfg to use (the DLC list)")
    ap.add_argument("--dry-run", action="store_true", help="check only, write nothing")
    a = ap.parse_args()
    try:
        configure(a.romfs, a.fonts, a.offset, a.aoc, dry_run=a.dry_run)
    except Error as e:
        print(f"ERROR: {e}")
        return 1
    print("RomFS header, offset and six font files validated. Dry run: nothing written." if a.dry_run
          else "Game-data setup complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
