#!/usr/bin/env python3
"""Minimal reader for the game's data.arc: file lookup by path, extraction.

Usage:
  arc.py info [--romfs-cfg <romfs.cfg>]
  arc.py get  <arc path>... --out <dir> [--romfs-cfg <romfs.cfg>]
  arc.py nro  [--out <dir>] [--romfs-cfg <romfs.cfg>]

data.arc is read in place inside the game's RomFS, found through romfs.cfg (default
dump/modules/romfs.cfg, the file the runtime reads). Paths are the archive's own, e.g.
"prebuilt:/nro/release/lua2cpp_mario.nro"; `get` writes each file under its base name.
`nro` extracts every module in prebuilt:/nro/release/ that is not compiled in yet (not in
dump/nro/bases.txt) into dump/nro, where the next `recomp.py all` picks it up - the same
place the runtime saves an NRO the game hands over (runtime/hle_ro.c). The archive stores
only hashes of names, so `nro` knows a module by the names in NRO_NAMES.

Layout (smash-arc's naming): a header, then a zstd-compressed file system table; a path's
hash40 (crc32 of the lowercase path | length << 32) picks a bucket, a binary search in the
bucket gives the path, and path -> file info index -> file info -> info-to-data -> folder
offset + file data locate the bytes.
"""
import argparse
import os
import struct
import sys
import zlib
from compression import zstd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nca import romfs_entries  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NRO_DIR = "prebuilt:/nro/release/"
# Every module in NRO_DIR as of 11.0.0 (96): the fighters, plus common, item, element (shared by
# Pyra/Mythra), koopag (Giga Bowser), ptrainer, and the Mii enemies of Spirits/Classic.
NRO_NAMES = ("bayonetta brave buddy captain chrom cloud common daisy dedede demon diddy dolly donkey duckhunt "
             "edge eflame element elight falco fox gamewatch ganon gaogaen gekkouga ike inkling item jack kamui "
             "ken kirby koopa koopag koopajr krool link littlemac lucario lucas lucina luigi mario mariod marth "
             "master metaknight mewtwo miienemyf miienemyg miienemys miifighter miigunner miiswordsman murabito "
             "nana ness pacman packun palutena peach pfushigisou pichu pickel pikachu pikmin pit pitb plizardon "
             "popo ptrainer purin pzenigame reflet richter ridley robot rockman rosetta roy ryu samus samusd "
             "sheik shizue shulk simon snake sonic szerosuit tantan toonlink trail wario wiifit wolf yoshi "
             "younglink zelda").split()
MAGIC = 0xABCDEF9876543210
LOW40 = (1 << 40) - 1


def hash40(path):
    s = path.lower().encode()
    return zlib.crc32(s) | (len(s) << 32)


class _Image:
    """The RomFS image inside the NCA, as nca.romfs_entries reads it."""
    start = data_off = 0
    enc_type = 1

    def __init__(self, f, base):
        self.f, self.base = f, base

    def read(self, off, size):
        self.f.seek(self.base + off)
        return self.f.read(size)


class Arc:
    def __init__(self, romfs_cfg):
        with open(romfs_cfg) as f:
            nca_path, base = f.read().split("\n")[:2]
        self.f = open(nca_path, "rb")
        image = _Image(self.f, int(base))
        found = [off for path, off, _ in romfs_entries(image) if path == "data.arc"]
        if not found:
            raise SystemExit(f"no data.arc in the RomFS named by {romfs_cfg}")
        self.base = int(base) + found[0]
        hdr = struct.unpack("<7Q", self.read(0, 0x38))
        if hdr[0] != MAGIC:
            raise SystemExit("data.arc: bad magic")
        self.file_section = hdr[2]
        data_start, decomp, comp, _ = struct.unpack("<4I", self.read(hdr[4], 16))
        t = self.t = zstd.decompress(self.read(hdr[4] + data_start, comp))
        if len(t) != decomp:
            raise SystemExit("data.arc: file system table did not decompress whole")

        (_size, paths, info_indices, folders, offsets_1, child_hashes, infos, info_to_datas, datas,
         offsets_2, datas_2) = struct.unpack_from("<11I", t, 0)
        regions, _, _, self.version, extra_folders, extra, _, _, extra_2, _ = struct.unpack_from("<BBHIIIIIII", t, 56)
        o = 0x58 + regions * 12                      # header, then one 12-byte entry per region
        quick_dirs, stream_hashes, stream_indices, stream_datas = struct.unpack_from("<4I", t, o)
        o += 16 + quick_dirs * 12 + stream_hashes * (8 + 12) + stream_indices * 4 + stream_datas * 16
        groups, self.buckets = struct.unpack_from("<2I", t, o)
        self.bucket_table = o + 8
        o = self.bucket_table + self.buckets * 8
        self.tab = {}   # name -> (offset, entry size)
        for name, count, size in (("hash_to_path", groups, 8), ("paths", paths, 32), ("info_indices", info_indices, 8),
                                  ("dir_hash_to_info", folders, 8), ("dirs", folders, 0x34),
                                  ("folder_offsets", offsets_1 + offsets_2 + extra_folders, 0x1C),
                                  ("child_hashes", child_hashes, 8), ("infos", infos + datas_2 + extra, 16),
                                  ("info_to_datas", info_to_datas + datas_2 + extra_2, 12),
                                  ("datas", datas + datas_2 + extra, 16)):
            self.tab[name] = (o, size)
            o += count * size

    def read(self, off, size):
        self.f.seek(self.base + off)
        return self.f.read(size)

    def _entry(self, table, index, fmt):
        offset, size = self.tab[table]
        return struct.unpack_from(fmt, self.t, offset + index * size)

    def locate(self, path):
        """(offset in data.arc, stored size, size, compressed) of a file, or None if absent."""
        h = hash40(path)
        start, count = struct.unpack_from("<2I", self.t, self.bucket_table + (h % self.buckets) * 8)
        lo, hi = start, start + count
        while lo < hi:
            mid = (lo + hi) // 2
            v = self._entry("hash_to_path", mid, "<Q")[0]
            if v & LOW40 == h:
                break
            lo, hi = (mid + 1, hi) if v & LOW40 < h else (lo, mid)
        else:
            return None
        full = self._entry("paths", v >> 40, "<Q")[0]
        info = self._entry("info_indices", full >> 40, "<2I")[1]
        _, _, to_data, _flags = self._entry("infos", info, "<4I")   # regional files are not handled
        folder, data, _ = self._entry("info_to_datas", to_data, "<3I")
        folder_offset = self._entry("folder_offsets", folder, "<Q")[0]
        in_folder, stored, size, flags = self._entry("datas", data, "<4I")
        return self.file_section + folder_offset + in_folder * 4, stored, size, bool(flags & 3)

    def get(self, path):
        where = self.locate(path)
        if where is None:
            return None
        off, stored, size, compressed = where
        data = self.read(off, stored)
        if compressed:
            data = zstd.decompress(data)
        if len(data) != size:
            raise SystemExit(f"{path}: {len(data)} bytes, the table says {size}")
        return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "get", "nro"])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--out")
    ap.add_argument("--romfs-cfg", default=os.path.join(ROOT, "dump", "modules", "romfs.cfg"))
    a = ap.parse_args()
    arc = Arc(a.romfs_cfg)
    if a.cmd == "info":
        v = arc.version
        print(f"data.arc version {v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}, {arc.buckets} buckets")
    elif a.cmd == "get":
        if not a.out:
            raise SystemExit("get needs --out <dir>")
        os.makedirs(a.out, exist_ok=True)
        for path in a.paths:
            data = arc.get(path)
            if data is None:
                print(f"  {path}: not in the archive")
                continue
            with open(os.path.join(a.out, path.rsplit("/", 1)[-1]), "wb") as f:
                f.write(data)
            print(f"  {path} ({len(data)})")
    elif a.cmd == "nro":
        out = a.out or os.path.join(ROOT, "dump", "nro")
        manifest = os.path.join(out, "bases.txt")
        known = {l.split()[0] for l in open(manifest)} if os.path.exists(manifest) else set()
        os.makedirs(out, exist_ok=True)
        for name in NRO_NAMES:
            module = "lua2cpp_" + name
            if module in known or os.path.exists(os.path.join(out, module + ".nro")):
                continue
            data = arc.get(NRO_DIR + module + ".nro")
            if data is None:
                continue                              # a fighter newer than this data.arc
            with open(os.path.join(out, module + ".nro"), "wb") as f:
                f.write(data)
            print(f"  {module}.nro ({len(data)})")


if __name__ == "__main__":
    main()
