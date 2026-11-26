#!/usr/bin/env python3
"""Prototype of the mod overlay of data.arc (openspec change mod-support, task 1): the experiment
that checks the game accepts a data.arc grown past its end, before the runtime builds one itself.

Usage:
  mod_arc_proto.py <mods dir> --out <overlay file> [--romfs-cfg <romfs.cfg>]
                   [--slot <fighter>.c0X=c0Y ...]

--slot loads a skin into another color of its fighter (openspec mod-support, decision 14): the
mod's paths are rewritten in memory, its files are not touched. Without --slot, a mods folder
whose name ends in -c0Y and whose files touch one fighter and one color goes to c0Y.

<mods dir> holds files at data.arc paths (fighter/shizue/model/body/c00/...). The overlay file
is what the runtime's experiment hook (SSBU_ARC_OVERLAY) served; the runtime now builds the same
table itself from mods/ (runtime/mod_arc.c, the same steps), and this stays as its reference:

  "SSBUARCO", u64 size of data.arc (N), u64 where the blob goes (B >= N), the 0x38-byte header
  with its table offset moved, then the blob: the new file system table (its 16-byte
  compressed-table header and the table in a zstd frame of raw blocks) and the mods' files.

The data.arc the game then sees is B + len(blob) bytes: the patched header, the original bytes,
zeros up to B, the blob. In the new table, each replaced path's info-to-data points at a new
FileData (uncompressed, the mod's size) in a new folder offset entry that covers the blob, so a
FileData several paths share keeps serving the others. New FileData entries go at the end of
the FileData table, which grows by its last ("extra") count; that count sizes the FileInfo table
too, so as many unused FileInfo entries are appended there.
"""
import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arc import Arc, LOW40, hash40, zstd_decompress  # noqa: E402
from nca import RomfsImage, romfs_entries  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW_BLOCK = 128 * 1024


REAL_ZSTD = False   # (--zstd: compress the table for real, to tell the two apart in the experiment)


def zstd_raw_frame(data, file=False):
    """`data` as a zstd frame of raw (stored) blocks: valid for any zstd decoder. A file's frame
    has the header the game's own files have (no content size, a 2 MiB window: the game
    decompresses files as a stream); the table's, a single segment with its size."""
    if REAL_ZSTD and not file:
        from compression.zstd import compress
        return compress(data)
    if file and os.environ.get("PROTO_REALFILE"):
        from compression.zstd import compress, CompressionParameter as P
        return compress(data, options={P.window_log: 21, P.content_size_flag: 0, P.checksum_flag: 0, P.compression_level: 3})
    if file:
        out = bytearray(struct.pack("<IBB", 0xFD2FB528, 0x00, 0x58))
    else:
        out = bytearray(struct.pack("<IB", 0xFD2FB528, 0xE0))   # single segment, 8-byte content size
        out += struct.pack("<Q", len(data))
    pos = 0
    while True:
        chunk = data[pos:pos + RAW_BLOCK]
        pos += len(chunk)
        last = pos >= len(data)
        head = (len(chunk) << 3) | (0 << 1) | int(last)
        out += head.to_bytes(3, "little") + chunk
        if last:
            return bytes(out)


def path_index(arc, path):
    h = hash40(path)
    start, count = struct.unpack_from("<2I", arc.t, arc.bucket_table + (h % arc.buckets) * 8)
    for k in range(start, start + count):
        v = arc._entry("hash_to_path", k, "<Q")[0]
        if v & LOW40 == h:
            return v >> 40
    return None


SLOT_RE = re.compile(r"^c0[0-7]$")


def slot_of(path):
    """(fighter, color "c0X") a path belongs to, or None: fighter/<f>/.../c0X/..., and the
    portraits ui/replace*/chara/chara_N/chara_N_<f>_0X.bntx."""
    parts = path.split("/")
    if len(parts) > 2 and parts[0] == "fighter":
        for c in parts[2:-1]:
            if re.fullmatch(r"c\d\d+", c):
                return parts[1], c
    m = re.fullmatch(r"ui/replace(?:_patch)?/chara/chara_\d+/chara_\d+_(\w+?)_(\d\d)\.bntx", path)
    if m:
        return m.group(1), "c" + m.group(2)
    return None


def move_path(path, fighter, src, dst):
    """`path` with its color `src` of `fighter` turned into `dst` (both "c0X"); others as they are."""
    parts = path.split("/")
    if len(parts) > 2 and parts[0] == "fighter" and parts[1] == fighter:
        return "/".join(dst if (0 < i < len(parts) - 1 and c == src) else c for i, c in enumerate(parts))
    m = re.fullmatch(r"(ui/replace(?:_patch)?/chara/chara_\d+/chara_\d+_)(\w+?)_(\d\d)(\.bntx)", path)
    if m and m.group(2) == fighter and "c" + m.group(3) == src:
        return f"{m.group(1)}{fighter}_{dst[1:]}{m.group(4)}"
    return path


def apply_slots(mods, slots, name):
    """{path: bytes} with the mod moved: `slots` is {(fighter, "c0X"): "c0Y"}; without any, a
    folder `name` ending in -c0Y moves a mod of one fighter and one color there."""
    touched = {slot_of(p) for p in mods} - {None}
    if not slots:
        m = re.search(r"-(c\d\d)$", name.lower())
        if m and len(touched) == 1:
            slots = {next(iter(touched)): m.group(1)}
            print(f"  color from the folder name: {slots}")
    for (f, src), dst in list(slots.items()):
        if not SLOT_RE.match(src) or not SLOT_RE.match(dst):
            print(f"  [mods] {name}: {f}.{src}={dst}: only c00-c07; left where it is", file=sys.stderr)
            del slots[(f, src)]
        elif (f, src) not in touched:
            print(f"  [mods] {name}: no files of {f} {src}", file=sys.stderr)
    if not slots:
        return mods
    out = {}
    for path, data in mods.items():
        new = path
        for (f, src), dst in slots.items():
            moved = move_path(path, f, src, dst)
            if moved != path:
                new = moved
                break
        if path.endswith("config.json"):   # its paths and directories move too
            text = data.decode("utf-8", "replace")
            def sub(m):
                p = m.group(1)
                for (f, src), dst in slots.items():
                    q = move_path(p, f, src, dst)
                    if q == p and p == f"fighter/{f}/{src}":   # the color's own directory
                        q = f"fighter/{f}/{dst}"
                    if q != p:
                        return json.dumps(q)
                return m.group(0)
            data = re.sub(r'"([^"]+)"', sub, text).encode()
        if new in out:
            raise SystemExit(f"{name}: {path} and another file both go to {new}")
        out[new] = data
    moved = sum(1 for p in mods if p not in out)
    print(f"  {moved} paths moved: " + ", ".join(f"{f} {s} -> {d}" for (f, s), d in slots.items()))
    return out


def raw_frame_size(n):
    blocks = max(1, (n + RAW_BLOCK - 1) // RAW_BLOCK)
    return 4 + 1 + 8 + 3 * blocks + n


def child_map(arc):
    """FileInfo index -> [(DirInfo index, child number)] for every mass-loading directory's
    children (DirInfo.file_info_start_index/file_count)."""
    t = arc.t
    o, size, count = arc.tab["dirs"]
    out = {}
    for d in range(count):
        start, n = struct.unpack_from("<2I", t, o + d * size + 0x20)
        for k in range(n):
            out.setdefault(start + k, []).append((d, k))
    return out


def build(arc, arc_size, mods):
    """(header, segments, blob, size) for {path: bytes}.

    A directory (DirInfo, "mass loading group") loads its children: child k is FileInfo
    file_info_start_index + k, and its bytes are FileData file_start_index + k of the directory's
    folder (folder_offsets[DirInfo.path.index]), read in one pass through the folder's region. A
    child shared with another costume instead points (load type 3) into the shared group, and its
    own FileData there is an empty placeholder. A mod's file goes into the child's own FileData
    (filled in, for a shared one, which then stops being shared: load type 1, its own FileInfoIndex)
    inside its folder, rebuilt past data.arc's end with the files in index order and the mod's in
    its place."""
    t = bytearray(arc.t)
    tab = arc.tab
    base = (arc_size + 0xFFF) & ~0xFFF
    i2d_o, i2d_s, _ = tab["info_to_datas"]
    fo_o, fo_s, _ = tab["folder_offsets"]
    d_o, d_s, _ = tab["datas"]
    inf_o, inf_s, _ = tab["infos"]
    dir_o, dir_s, dir_n = tab["dirs"]
    p_o, p_s, _ = tab["paths"]
    ii_o, ii_s, ii_n = tab["info_indices"]
    children = child_map(arc)

    by_path_info = {}   # path index -> child FileInfo indices
    for info in children:
        pi = struct.unpack_from("<I", t, inf_o + info * inf_s)[0]
        by_path_info.setdefault(pi, []).append(info)

    jobs = {}       # folder -> {FileData index: (bytes, real size)}
    new_ii = []     # new FileInfoIndex entries
    new_paths = []  # new FilePath entries
    p_n = tab["paths"][2]
    unshared = 0
    reshared = 0

    def place(info, d, k):
        """(folder, own FileData, info-to-data index, its entry) of child k of directory d."""
        folder = struct.unpack_from("<Q", t, dir_o + d * dir_s)[0] >> 40
        start = struct.unpack_from("<QIIIII", t, fo_o + folder * fo_s)[3]
        i2d = struct.unpack_from("<4I", t, inf_o + info * inf_s)[2]
        return folder, start + k, i2d, struct.unpack_from("<3I", t, i2d_o + i2d * i2d_s)

    def unshare(info, d, k, pi):
        """Make child k of directory d (FileInfo `info`, path `pi`) read the directory's own FileData
        (its placeholder) with a FileInfoIndex of its own. Its sharing was load type 3 (a shared
        group) or 0 (another costume's own FileData)."""
        nonlocal unshared
        folder, own, i2d, (f, dd, fl) = place(info, d, k)
        ii = struct.unpack_from("<4I", t, inf_o + info * inf_s)[1]
        # load type 3 keeps its low 24 bits; an own file (load type 1) has 0 there
        struct.pack_into("<3I", t, i2d_o + i2d * i2d_s, folder, own, (1 << 24) | (fl & 0xFFFFFF if fl >> 24 == 3 else 0))
        shared_info = struct.unpack_from("<2I", t, ii_o + ii * ii_s)[1]
        if struct.unpack_from("<I", t, inf_o + shared_info * inf_s)[0] == pi:
            # this path is the shared file's source, which the other costumes still use: the
            # shared FileInfo gets a path of its own (a copy of this one that no lookup finds, its
            # length 0xFF) that keeps the shared FileInfoIndex
            full = struct.unpack_from("<Q", t, p_o + pi * p_s)[0]
            dummy = bytearray(t[p_o + pi * p_s:p_o + (pi + 1) * p_s])
            struct.pack_into("<Q", dummy, 0, (full & 0xFFFFFFFF) | (0xFF << 32) | (ii << 40))
            struct.pack_into("<I", t, inf_o + shared_info * inf_s, p_n + len(new_paths))
            new_paths.append(bytes(dummy))
        my_ii = ii_n + len(new_ii)
        new_ii.append(struct.pack("<2I", d, info))
        struct.pack_into("<I", t, inf_o + info * inf_s + 4, my_ii)
        if fl >> 24 == 0:   # it was marked shared (0x100000, 0x200000); now it is its own file
            flags = struct.unpack_from("<I", t, inf_o + info * inf_s + 12)[0]
            struct.pack_into("<I", t, inf_o + info * inf_s + 12, flags & ~0x300000)
        full = struct.unpack_from("<Q", t, p_o + pi * p_s)[0]
        struct.pack_into("<Q", t, p_o + pi * p_s, (full & LOW40) | (my_ii << 40))
        unshared += 1
        return folder, own

    referenced = {struct.unpack_from("<3I", t, i2d_o + i * i2d_s)[1] for i in range(tab["info_to_datas"][2])}

    def take_slot(own):
        """Whether a child's own FileData can be filled: empty, or with stale bytes no info-to-data
        reads (then emptied, to be laid out as a placeholder)."""
        if not struct.unpack_from("<4I", t, d_o + own * d_s)[1]:
            return True
        if own in referenced:
            return False
        struct.pack_into("<4I", t, d_o + own * d_s, 0, 0, 0, 0)
        return True

    modded = {}   # (folder, FileData) -> path index, of every FileData a mod replaces
    identical = 0
    for path, data in sorted(mods.items()):
        pi = path_index(arc, path)
        if pi is None:
            print(f"  {path}: not in data.arc (new files are not handled yet)", file=sys.stderr)
            continue
        infos = by_path_info.get(pi, [])
        if not infos:
            print(f"  {path}: no directory loads it (not handled yet)", file=sys.stderr)
            continue
        try:
            same = arc.get(path) == data
        except SystemExit:
            same = False
        if same:   # nothing to replace (mods often carry unchanged files)
            identical += 1
            continue
        for info in infos:
            for d, k in children[info]:
                folder, own, _i2d, (f, dd, fl) = place(info, d, k)
                if (f, dd) != (folder, own):
                    if fl >> 24 not in (0, 3):
                        print(f"  {path}: child {k} of directory {d} loads ({f}, {dd}) with load type {fl >> 24}; skipped", file=sys.stderr)
                        continue
                    if not take_slot(own):
                        print(f"  {path}: child {k} of directory {d} has no free FileData to take; skipped", file=sys.stderr)
                        continue
                    unshare(info, d, k, pi)
                jobs.setdefault(folder, {})[own] = (zstd_raw_frame(data, file=True), len(data))
                modded[(folder, own)] = pi

    # costumes that read a replaced FileData directly (load type 0, e.g. Steve c02's wing reading
    # c00's) keep the original: each gets a copy of it in its own placeholder, like ARCropolis'
    # reshare of a source's dependents
    by_i2d = {}
    for info in children:
        by_i2d.setdefault(struct.unpack_from("<4I", t, inf_o + info * inf_s)[2], []).append(info)
    for i in range(tab["info_to_datas"][2]):
        f, dd, fl = struct.unpack_from("<3I", t, i2d_o + i * i2d_s)
        if (f, dd) not in modded or fl >> 24 != 0:
            continue
        for info in by_i2d.get(i, []):
            dpi = struct.unpack_from("<I", t, inf_o + info * inf_s)[0]
            for d, k in children[info]:
                folder, own, _i2d, _e = place(info, d, k)
                if (folder, own) in modded:
                    continue
                if not take_slot(own):
                    print(f"  directory {d} child {k}: reads a replaced file and has no free FileData; it gets the mod's", file=sys.stderr)
                    continue
                where = arc._data(i)   # the original bytes, still those of the source
                raw = arc.read(where[0], where[1])
                original = zstd_decompress(raw) if where[3] & 3 else raw
                unshare(info, d, k, dpi)
                jobs.setdefault(folder, {})[own] = (zstd_raw_frame(original, file=True), len(original))
                reshared += 1
    print(f"  {identical} files equal to the original, left as they are")
    print(f"  {sum(len(j) for j in jobs.values())} FileData filled, {unshared} children unshared "
          f"({reshared} other costumes given the original), {len(jobs)} folders")

    users = {}   # folder -> FileData its info-to-datas use outside its own range
    for i in range(tab["info_to_datas"][2]):
        f, d, _fl = struct.unpack_from("<3I", t, i2d_o + i * i2d_s)
        if f in jobs:
            users.setdefault(f, set()).add(d)

    hdr = list(struct.unpack_from("<11I", t, 0))
    new_size = len(t) + 8 * len(new_ii) + (p_s + 8) * len(new_paths)
    table_end = (16 + raw_frame_size(new_size) + 0xFFF) & ~0xFFF
    segments = []          # (place in data.arc, size, source offset, source: 0 data.arc, 1 blob)
    blob_files = bytearray()
    at = base + table_end
    for folder, files in sorted(jobs.items()):
        f_off, decomp, size, start, count, res = struct.unpack_from("<QIIIII", t, fo_o + folder * fo_s)
        src = arc.file_section + f_off
        # the FileData this folder owns: those an info-to-data reads through it (a directory's
        # range, start..start+count, also covers its subfolders' files, read through their own
        # folders, whose offsets must stay as they are) and the placeholders being filled
        mine = users.get(folder, set()) | set(files)
        old = {i: struct.unpack_from("<4I", t, d_o + i * d_s) for i in mine}
        # the folder's files in their physical order (by offset; FileData sharing an offset are
        # one file), each empty placeholder being filled going right after the file that comes
        # before it by index; then everything laid out again, one after another
        real = {}   # old offset -> stored length
        for i, (off4, stored, _ds, _fl) in old.items():
            if stored:
                real[off4 * 4] = max(real.get(off4 * 4, 0), stored)
        after = {}  # old offset (or -1: the start) -> placeholders that follow it
        for i in sorted(files):
            if old[i][1]:
                continue
            prev = -1
            for j in range(i - 1, start - 1, -1):
                if j in old and old[j][1]:
                    prev = old[j][0] * 4
                    break
            after.setdefault(prev, []).append(i)
        replaced = {old[i][0] * 4: i for i in files if old[i][1]}
        new_at = {}   # old offset -> new offset
        place = {}
        pos = 0
        def put_blob(i):
            nonlocal pos
            body, real_size = files[i]
            segments.append((at + pos, len(body), len(blob_files), 1))
            blob_files.extend(body)
            place[i] = pos
            pos = (pos + len(body) + 0xF) & ~0xF
        for i in after.get(-1, []):
            put_blob(i)
        for o in sorted(real):
            if o in replaced:
                put_blob(replaced[o])
                new_at[o] = place[replaced[o]]
            else:
                new_at[o] = pos
                segments.append((at + pos, real[o], src + o, 0))
                pos = (pos + real[o] + 0xF) & ~0xF
            for i in after.get(o, []):
                put_blob(i)
        for i, (off4, stored, dsize, flags) in old.items():
            if i in files:
                body, real_size = files[i]
                if not os.environ.get("PROTO_KEEP_DECOMP"):
                    decomp += real_size - dsize
                struct.pack_into("<4I", t, d_o + i * d_s, place[i] // 4, len(body), real_size, (flags & ~3) | 3)
            elif stored:
                struct.pack_into("<I", t, d_o + i * d_s, new_at[off4 * 4] // 4)
        shift = pos - ((size + 0xF) & ~0xF)
        new_size_f = ((size + 0xF) & ~0xF) + shift
        struct.pack_into("<QIIIII", t, fo_o + folder * fo_s, at - arc.file_section, decomp, new_size_f, start, count, res)
        print(f"  folder {folder}: {len(files)} files, {size} bytes at {src:#x} rebuilt at {at:#x}, {new_size_f} bytes now")
        at = (at + new_size_f + 0xFFF) & ~0xFFF
    segments.sort()

    # the new paths also go into the hash lookup: each in its bucket, in hash order
    h_o, h_s, h_n = tab["hash_to_path"]
    lookup = [struct.unpack_from("<Q", t, h_o + i * h_s)[0] for i in range(h_n)]
    nb = arc.buckets
    buckets = [list(struct.unpack_from("<2I", t, arc.bucket_table + b * 8)) for b in range(nb)]
    for n, entry in enumerate(new_paths):
        h = struct.unpack_from("<Q", entry, 0)[0] & LOW40
        b = h % nb
        start, count = buckets[b]
        k = start
        while k < start + count and lookup[k] & LOW40 < h:
            k += 1
        lookup.insert(k, h | ((p_n + n) << 40))
        buckets[b][1] += 1
        for bb in range(b + 1, nb):
            buckets[bb][0] += 1
    for b in range(nb):
        struct.pack_into("<2I", t, arc.bucket_table + b * 8, *buckets[b])
    struct.pack_into("<I", t, arc.bucket_table - 8, h_n + len(new_paths))   # the lookup's count
    h_end = h_o + h_s * h_n
    ii_end = ii_o + ii_s * ii_n
    p_end = p_o + p_s * p_n
    assert h_end == p_o and p_end <= ii_end
    new_t = (t[:h_o] + b"".join(struct.pack("<Q", v) for v in lookup) + t[p_o:p_end] + b"".join(new_paths)
             + t[p_end:ii_end] + b"".join(new_ii) + t[ii_end:])
    assert len(new_t) == new_size
    hdr[0] = len(new_t)
    hdr[1] += len(new_paths)                 # file_info_path_count
    print(f"  {len(new_paths)} source paths kept for the other costumes")
    hdr[2] += len(new_ii)                    # file_info_index_count
    struct.pack_into("<11I", new_t, 0, *hdr)
    frame = zstd_raw_frame(bytes(new_t))
    table = struct.pack("<4I", 16, len(new_t), len(frame), 16 + len(frame)) + frame
    blob = table + blob_files
    segments = [(base, len(table), 0, 1)] + [(a, n, (o + len(table)) if s else o, s) for a, n, o, s in segments]
    header = bytearray(arc.read(0, 0x38))
    struct.pack_into("<Q", header, 0x20, base)   # the file system table, now at the blob's start
    return bytes(header), segments, blob, at


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mods")
    ap.add_argument("--out", required=True)
    ap.add_argument("--romfs-cfg", default=os.path.join(ROOT, "dump", "modules", "romfs.cfg"))
    ap.add_argument("--zstd", action="store_true")
    ap.add_argument("--slot", action="append", default=[], metavar="FIGHTER.C0X=C0Y",
                    help="load the mod's color c0X of FIGHTER as c0Y (repeatable)")
    a = ap.parse_args()
    global REAL_ZSTD
    REAL_ZSTD = a.zstd
    image = RomfsImage(a.romfs_cfg)
    (arc_off, arc_size), = [(off, size) for path, off, size in romfs_entries(image) if path == "data.arc"]
    arc = Arc(a.romfs_cfg)
    mods = {}
    for dirpath, _dirs, files in os.walk(a.mods):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, a.mods).replace(os.sep, "/").lower()
            with open(full, "rb") as f:
                mods[rel] = f.read()
    slots = {}
    for spec in a.slot:
        m = re.fullmatch(r"(\w+)\.(c\d+)=(c\d+)", spec.lower())
        if not m:
            raise SystemExit(f"--slot {spec}: expected <fighter>.c0X=c0Y")
        slots[(m.group(1), m.group(2))] = m.group(3)
    mods = apply_slots(mods, slots, os.path.basename(os.path.normpath(a.mods)))
    header, segments, blob, total = build(arc, arc_size, mods)
    with open(a.out, "wb") as f:
        f.write(b"SSBUARC2" + struct.pack("<QQ", arc_size, total) + header + struct.pack("<II", len(segments), 0))
        for seg in segments:
            f.write(struct.pack("<QQQII", *seg, 0))
        f.write(blob)
    print(f"data.arc {arc_size} bytes, {total} now; {len(segments)} segments, blob {len(blob)} bytes; {len(mods)} files")

    # check: the overlaid archive, read back with arc.py, gives the mods' bytes
    def read(off, size):
        out = bytearray()
        while size:
            if off < 0x38:
                n = min(size, 0x38 - off); out += header[off:off + n]
            elif off < arc_size:
                n = min(size, arc_size - off); out += image.read(arc_off + off, n)
            else:
                seg = [sg for sg in segments if sg[0] <= off < sg[0] + sg[1]]
                if not seg:
                    nxt = min([sg[0] for sg in segments if sg[0] > off] + [off + size])
                    n = min(size, nxt - off); out += bytes(n)
                else:
                    at, ln, src, kind = seg[0]
                    n = min(size, at + ln - off)
                    o = src + off - at
                    out += blob[o:o + n] if kind else image.read(arc_off + o, n)
            off += n; size -= n
        return bytes(out)
    check = Arc(read)
    for path, data in mods.items():
        if path_index(arc, path) is None:
            continue
        try:
            got = check.get(path)
        except Exception as e:   # (a broken frame: say where it is)
            print(f"  {path}: BROKEN {e}; at {check.locate(path)}")
            continue
        print(f"  {path}: {'ok' if got == data else 'MISMATCH'}")
    for path in os.environ.get("PROTO_CHECK_SAME", "").split():
        print(f"  {path} unchanged: {check.get(path) == arc.get(path)}")


if __name__ == "__main__":
    main()
