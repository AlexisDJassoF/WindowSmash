#!/usr/bin/env python3
"""Minimal NCA3 reader: header decryption, section listing, ExeFS (PFS0) and RomFS access.

Usage:
  nca.py info      <file.nca> --keys <dir>
  nca.py exefs     <file.nca> --keys <dir> --out <dir>
  nca.py romfs-ls  <file.nca> --keys <dir>
  nca.py romfs-get <file.nca> --keys <dir> --out <dir> [--match <substr>] [--flat]
  nca.py romfs-cfg <file.nca> --keys <dir> --out <romfs.cfg>
  nca.py aoc-cfg   <registered dir> --keys <dir> --out <aoc.cfg> [--app <title id>]

aoc-cfg lists the add-on content (DLC) installed in a registered-content folder (e.g. yuzu's
nand/user/Contents/registered), from the metadata of each package, for the runtime's nn::aoc.

The romfs-* commands also take a RomFS image on its own (a romfs.bin another tool wrote, e.g.
the game's RomFS with its update applied) in place of the NCA; that needs no keys.

Key material is read from <dir>/prod.keys and <dir>/title.keys and is never printed.
"""
import argparse
import os
import struct
import sys

from Cryptodome.Cipher import AES

MEDIA_UNIT = 0x200
CONTENT_TYPES = {0: "Program", 1: "Meta", 2: "Control", 3: "Manual", 4: "Data", 5: "PublicData"}
ENC_TYPES = {0: "Auto", 1: "None", 2: "XTS", 3: "CTR", 4: "BKTR"}
FS_TYPES = {0: "RomFS", 1: "PartitionFS"}


def load_keys(path):
    keys = {}
    if not os.path.exists(path):
        return keys
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            try:
                keys[k.strip().lower()] = bytes.fromhex(v.strip())
            except ValueError:
                pass
    return keys


def xts_decrypt(key32, data, sector0=0, sector_size=MEDIA_UNIT):
    """AES-128-XTS with Nintendo's big-endian sector tweak."""
    k1 = AES.new(key32[:16], AES.MODE_ECB)
    k2 = AES.new(key32[16:], AES.MODE_ECB)
    mask = (1 << 128) - 1
    out = bytearray()
    for s in range(len(data) // sector_size):
        t = int.from_bytes(k2.encrypt((sector0 + s).to_bytes(16, "big")), "little")
        sec = data[s * sector_size:(s + 1) * sector_size]
        for b in range(0, sector_size, 16):
            c = int.from_bytes(sec[b:b + 16], "little") ^ t
            p = int.from_bytes(k1.decrypt(c.to_bytes(16, "little")), "little") ^ t
            out += p.to_bytes(16, "little")
            t <<= 1
            if t >> 128:
                t = (t & mask) ^ 0x87
    return bytes(out)


class Section:
    def __init__(self, nca, index, start, end, fsh):
        self.nca = nca
        self.index = index
        self.start = start
        self.end = end
        self.fs_type = fsh[0x02]
        self.hash_type = fsh[0x03]
        self.enc_type = fsh[0x04]
        self.ctr_hi = bytes(reversed(fsh[0x140:0x148]))
        if self.hash_type == 2:      # HierarchicalSha256 -> PFS0
            self.data_off, self.data_size = struct.unpack_from("<QQ", fsh, 0x40)
        elif self.hash_type == 3:    # HierarchicalIntegrity (IVFC) -> RomFS in level 6
            self.data_off, self.data_size = struct.unpack_from("<QQ", fsh, 0x18 + 5 * 0x18)
        else:
            self.data_off, self.data_size = 0, end - start

    def read_raw(self, off, size):
        """Read decrypted bytes at an offset relative to the section start."""
        if self.enc_type == 1 or self.nca.plaintext:
            self.nca.f.seek(self.start + off)
            return self.nca.f.read(size)
        if self.enc_type != 3:
            raise NotImplementedError(f"encryption type {ENC_TYPES.get(self.enc_type, self.enc_type)}")
        absolute = self.start + off
        aligned = absolute & ~0xF
        pad = absolute - aligned
        length = (pad + size + 0xF) & ~0xF
        self.nca.f.seek(aligned)
        iv = self.ctr_hi + (aligned >> 4).to_bytes(8, "big")
        cipher = AES.new(self.nca.body_key, AES.MODE_CTR, nonce=b"", initial_value=int.from_bytes(iv, "big"))
        return cipher.decrypt(self.nca.f.read(length))[pad:pad + size]

    def read(self, off, size):
        """Read from the section's payload (the PFS0 / RomFS image)."""
        return self.read_raw(self.data_off + off, size)


class Nca:
    def __init__(self, path, keydir):
        self.path = path
        self.f = open(path, "rb")
        prod = load_keys(os.path.join(keydir, "prod.keys"))
        title = load_keys(os.path.join(keydir, "title.keys"))
        if "header_key" not in prod:
            raise SystemExit("prod.keys has no header_key")
        raw = self.f.read(0xC00)
        # some dumps store the whole content already decrypted, header and sections alike
        self.plaintext = raw[0x200:0x204] == b"NCA3"
        hdr = raw if self.plaintext else xts_decrypt(prod["header_key"], raw)
        self.magic = hdr[0x200:0x204]
        if self.magic != b"NCA3":
            raise SystemExit(f"unsupported NCA magic {self.magic!r} (bad header_key?)")
        self.content_type = hdr[0x205]
        self.kaek_index = hdr[0x207]
        self.size, self.title_id = struct.unpack_from("<QQ", hdr, 0x208)
        self.sdk_version = struct.unpack_from("<I", hdr, 0x21C)[0]
        gen = max(hdr[0x206], hdr[0x220])
        self.key_gen = gen - 1 if gen else 0
        self.rights_id = hdr[0x230:0x240]
        self.has_rights = any(self.rights_id)

        if self.has_rights:
            tk = title.get(self.rights_id.hex())
            kek = prod.get(f"titlekek_{self.key_gen:02x}")
            if tk is None or kek is None:
                raise SystemExit("missing title key or titlekek for this content")
            self.body_key = AES.new(kek, AES.MODE_ECB).decrypt(tk)
        else:
            name = ("application", "ocean", "system")[self.kaek_index]
            kak = prod.get(f"key_area_key_{name}_{self.key_gen:02x}")
            if kak is None:
                raise SystemExit(f"missing key_area_key_{name}_{self.key_gen:02x}")
            area = AES.new(kak, AES.MODE_ECB).decrypt(hdr[0x300:0x340])
            self.body_key = area[0x20:0x30]

        self.sections = []
        for i in range(4):
            s, e = struct.unpack_from("<II", hdr, 0x240 + i * 0x10)
            if not e:
                continue
            fsh = hdr[0x400 + i * 0x200:0x600 + i * 0x200]
            self.sections.append(Section(self, i, s * MEDIA_UNIT, e * MEDIA_UNIT, fsh))

    def section(self, fs_type):
        for s in self.sections:
            if s.fs_type == fs_type:
                return s
        return None


class PlainRomfs:
    """A RomFS image in a file of its own, read in place."""
    def __init__(self, path):
        self.f = open(path, "rb")
        self.start = self.data_off = 0
        self.enc_type = 1

    def read(self, off, size):
        self.f.seek(off)
        return self.f.read(size)


def is_plain_romfs(path):
    with open(path, "rb") as f:
        head = f.read(8)
    return len(head) == 8 and struct.unpack("<Q", head)[0] == 0x50


def pfs0_entries(sec):
    head = sec.read(0, 0x10)
    if head[:4] != b"PFS0":
        raise SystemExit(f"section {sec.index}: not a PFS0 ({head[:4]!r}) - wrong key?")
    count, strsz = struct.unpack_from("<II", head, 4)
    table = sec.read(0x10, count * 0x18 + strsz)
    strings = table[count * 0x18:]
    base = 0x10 + count * 0x18 + strsz
    for i in range(count):
        off, size, name_off = struct.unpack_from("<QQI", table, i * 0x18)
        name = strings[name_off:strings.index(b"\0", name_off)].decode()
        yield name, base + off, size


def romfs_entries(sec):
    hdr = sec.read(0, 0x50)
    (hsz, _dho, _dhs, dmo, dms, _fho, _fhs, fmo, fms, fdo) = struct.unpack("<10Q", hdr)
    if hsz != 0x50:
        raise SystemExit("section is not a RomFS image")
    dirs = sec.read(dmo, dms)
    files = sec.read(fmo, fms)
    none = 0xFFFFFFFF

    def walk(doff, prefix):
        _parent, _sib, child, ffile, _h, nlen = struct.unpack_from("<6I", dirs, doff)
        name = dirs[doff + 0x18:doff + 0x18 + nlen].decode("utf-8", "replace")
        path = prefix + name + ("/" if name else "")
        fo = ffile
        while fo != none:
            _fp, fsib, doff2, dsize, _fh, fnlen = struct.unpack_from("<IIQQII", files, fo)
            fname = files[fo + 0x20:fo + 0x20 + fnlen].decode("utf-8", "replace")
            yield path + fname, fdo + doff2, dsize
            fo = fsib
        co = child
        while co != none:
            yield from walk(co, path)
            co = struct.unpack_from("<I", dirs, co + 4)[0]

    yield from walk(0, "")


def copy_out(sec, off, size, dest, chunk=64 << 20):
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    done = 0
    with open(dest, "wb") as o:
        while done < size:
            n = min(chunk, size - done)
            o.write(sec.read(off + done, n))
            done += n
            if size > chunk:
                print(f"\r  {dest}: {done * 100 // size}%", end="", file=sys.stderr)
    if size > chunk:
        print(file=sys.stderr)


def aoc_cfg(root, keydir, out, app):
    """Write "<index> <title id> ..." for each AddOnContent package of one application."""
    found = {}   # application id -> [(index, line)]
    for dp, _dirs, files in os.walk(root):
        for f in files:
            if not f.endswith(".nca"):
                continue
            try:
                nca = Nca(os.path.join(dp, f), keydir)
            except SystemExit:   # content under a title key: not metadata
                continue
            if nca.content_type != 1:
                continue
            sec = nca.section(1)
            for name, off, size in pfs0_entries(sec):
                if not name.endswith(".cnmt"):
                    continue
                cnmt = sec.read(off, size)
                title, version, kind = struct.unpack_from("<QIB", cnmt, 0)
                if kind != 0x82:   # AddOnContent; extended header { u64 application id; u32 required version }
                    continue
                owner, required = struct.unpack_from("<QI", cnmt, 0x20)
                index = title - (owner + 0x1000)   # the index counts from the application's AOC base id
                found.setdefault(owner, []).append(
                    (index, f"{index} {title:016X} v{version >> 16} needs application v{required >> 16}\n"))
    if app is None:
        if len(found) > 1:
            raise SystemExit("add-on content of several applications: pick one with --app " +
                             " ".join(f"{a:016X}" for a in found))
        app = next(iter(found), None)
    entries = sorted(found.get(app, []))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", newline="\n") as f:
        f.write(f"# add-on content of {app or 0:016X}: <index> <title id> <version> (tools/nca.py aoc-cfg)\n")
        f.writelines(line for _, line in entries)
    print(f"wrote {out}: {len(entries)} add-on contents")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "exefs", "romfs-ls", "romfs-get", "romfs-cfg", "aoc-cfg"])
    ap.add_argument("nca")
    ap.add_argument("--keys")
    ap.add_argument("--out")
    ap.add_argument("--match", default="")
    ap.add_argument("--flat", action="store_true", help="romfs-get: every file straight into --out, without its folders")
    ap.add_argument("--app", type=lambda s: int(s, 16), help="aoc-cfg: the application whose add-on content to list")
    a = ap.parse_args()

    if a.cmd == "aoc-cfg":
        if not (a.keys and a.out):
            raise SystemExit("aoc-cfg needs --keys <dir> and --out <aoc.cfg>")
        return aoc_cfg(a.nca, a.keys, a.out, a.app)
    if is_plain_romfs(a.nca):
        if not a.cmd.startswith("romfs"):
            raise SystemExit(f"{a.nca} is a RomFS image, not an NCA: only the romfs-* commands apply")
        romfs = PlainRomfs(a.nca)
        nca = type("PlainImage", (), {"section": lambda self, fs_type: romfs, "plaintext": True})()
    else:
        if not a.keys:
            raise SystemExit("--keys <dir> is needed to read an NCA")
        nca = Nca(a.nca, a.keys)
    if a.cmd == "info":
        print(f"type={CONTENT_TYPES.get(nca.content_type, nca.content_type)} title_id={nca.title_id:016X} "
              f"size={nca.size} key_gen={nca.key_gen} titlekey_crypto={nca.has_rights} "
              f"sdk={nca.sdk_version >> 24}.{(nca.sdk_version >> 16) & 0xFF}.{(nca.sdk_version >> 8) & 0xFF}")
        for s in nca.sections:
            print(f"  section {s.index}: {FS_TYPES.get(s.fs_type, s.fs_type):<12} enc={ENC_TYPES.get(s.enc_type, s.enc_type):<5} "
                  f"file=[{s.start:#x},{s.end:#x}) payload_off={s.data_off:#x} payload_size={s.data_size:#x}")
    elif a.cmd == "exefs":
        sec = nca.section(1)
        for name, off, size in pfs0_entries(sec):
            print(f"  {name:<12} {size:>10}")
            copy_out(sec, off, size, os.path.join(a.out, name))
    elif a.cmd == "romfs-ls":
        total = 0
        for path, _off, size in romfs_entries(nca.section(0)):
            total += size
            print(f"{size:>14}  {path}")
        print(f"total {total} bytes")
    elif a.cmd == "romfs-cfg":   # where the runtime finds the RomFS: "<nca path>\n<byte offset>\n"
        sec = nca.section(0)
        if sec.enc_type != 1 and not nca.plaintext:
            raise SystemExit("the RomFS section is encrypted; the runtime reads it in place only when it is not")
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, "w") as f:
            f.write(f"{os.path.abspath(a.nca)}\n{sec.start + sec.data_off}\n")
        print(f"wrote {a.out}: RomFS at byte {sec.start + sec.data_off:#x} of {a.nca}")
    elif a.cmd == "romfs-get":
        sec = nca.section(0)
        for path, off, size in romfs_entries(sec):
            if a.match in path:
                print(f"  {path} ({size})")
                copy_out(sec, off, size, os.path.join(a.out, os.path.basename(path) if a.flat else path.replace("/", os.sep)))


if __name__ == "__main__":
    main()
